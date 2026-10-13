// indexcraft_bench: reproducible benchmark runner for the IndexCraft storage engine.
//
// For each dataset size it measures (all numbers come from this run, nothing is canned):
//   insert_heap_only        heap-file append only, random key order (no index, no duplicate check)
//   insert_index_only       in-memory B+ Tree insert only, random key order
//   insert_full             Database::insert (duplicate probe + heap + index), random key order
//   insert_full_ascending   Database::insert with ascending keys into a fresh database
//   index_maintenance_overhead   derived: insert_full minus insert_heap_only, per record
//   index_rebuild_on_open   close + reopen: validate the heap file and rebuild the B+ Tree
//   point_lookup            WHERE key = <existing key>        (index vs full scan)
//   full_scan               predicate that matches nothing    (visits every record)
//   range_0.1pct .. range_50pct   WHERE key >= a AND key <= b (index vs full scan)
//
// Timing boundary: std::chrono::steady_clock around QueryEngine::execute_plan() with a prepared
// plan, so parsing and planning are excluded. Rows are materialised in both paths. Insert phases
// are timed in batches of 1000 inserts (payload generation excluded).
// Cache condition: warm OS page cache (files were just written; caches are not dropped).
//
// Output (in --out): summary.csv, raw.csv, meta.json. Progress is printed as JSON lines on stdout.

#include <sys/utsname.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "indexcraft/bplus_tree.hpp"
#include "indexcraft/build_config.hpp"
#include "indexcraft/build_info.hpp"
#include "indexcraft/database.hpp"
#include "indexcraft/heap_file.hpp"
#include "indexcraft/json.hpp"
#include "indexcraft/query.hpp"

using namespace indexcraft;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct Config {
  std::vector<std::uint64_t> sizes{10000, 100000};
  std::size_t reps = 1000;        // index point lookups
  std::size_t scan_reps = 20;     // any full-scan based measurement
  std::size_t range_reps = 20;    // index range queries (0.1% and 1%)
  std::size_t heavy_reps = 5;     // 10% and 50% ranges (both paths)
  std::size_t warmup = 3;
  std::size_t max_keys = 64;
  std::uint64_t seed = 42;
  std::string workdir = "bench_work";
  std::string out = "bench_results";
  std::string run_id;
};

struct Failure {
  std::string message;
};

void emit_progress(const std::string& stage, std::uint64_t size, double pct) {
  std::printf("{\"event\":\"progress\",\"stage\":%s,\"dataset_size\":%llu,\"percent\":%.1f}\n",
              json_quote(stage).c_str(), static_cast<unsigned long long>(size), pct);
  std::fflush(stdout);
}

std::string utc_now() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::uint64_t splitmix(std::uint64_t& s) {
  std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// Deterministic variable-length payload: 16..111 bytes (mean ~64), printable characters.
std::string make_payload(std::uint64_t seed, Key key) {
  std::uint64_t s = seed ^ (static_cast<std::uint64_t>(key) * 0xD6E8FEB86659FD93ULL);
  std::size_t len = 16 + splitmix(s) % 96;
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
  std::string p(len, ' ');
  for (char& c : p) c = alphabet[splitmix(s) % (sizeof(alphabet) - 1)];
  return p;
}

// Random permutation of 1..n (Fisher-Yates with mt19937_64; reproducible on every platform).
std::vector<Key> shuffled_keys(std::uint64_t n, std::uint64_t seed) {
  std::vector<Key> keys(n);
  std::iota(keys.begin(), keys.end(), 1);
  std::mt19937_64 rng(seed);
  for (std::uint64_t i = n - 1; i > 0; --i) std::swap(keys[i], keys[rng() % (i + 1)]);
  return keys;
}

struct Summary {
  double mean = 0, median = 0, p90 = 0, p95 = 0, p99 = 0, min = 0, max = 0, stddev = 0;
};

double percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0;
  std::size_t rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
  rank = std::min(std::max<std::size_t>(rank, 1), sorted.size());
  return sorted[rank - 1];
}

Summary summarize(std::vector<double> v) {
  Summary s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  double sum = std::accumulate(v.begin(), v.end(), 0.0);
  s.mean = sum / static_cast<double>(v.size());
  double var = 0;
  for (double x : v) var += (x - s.mean) * (x - s.mean);
  s.stddev = v.size() > 1 ? std::sqrt(var / static_cast<double>(v.size() - 1)) : 0.0;
  s.min = v.front();
  s.max = v.back();
  s.median = percentile(v, 50);
  s.p90 = percentile(v, 90);
  s.p95 = percentile(v, 95);
  s.p99 = percentile(v, 99);
  return s;
}

std::string csv_escape(const std::string& s) {
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string o = "\"";
  for (char c : s) { if (c == '"') o += "\"\""; else o.push_back(c); }
  return o + "\"";
}

class Recorder {
 public:
  Recorder(const std::string& dir, const std::string& run_id) : run_id_(run_id) {
    fs::create_directories(dir);
    summary_.open(dir + "/summary.csv");
    raw_.open(dir + "/raw.csv");
    if (!summary_ || !raw_) throw Failure{"cannot write result files in " + dir};
    summary_ << "run_id,dataset_size,workload,access_path,repetitions,mean_ns,median_ns,p90_ns,p95_ns,p99_ns,min_ns,max_ns,stddev_ns,throughput_ops_s,mean_records_examined,notes\n";
    raw_ << "run_id,dataset_size,workload,access_path,rep,ops,latency_ns\n";
  }

  // `latencies_ns[i]` is the time of rep i covering `ops_per_rep[i]` operations (1 for queries).
  void record(std::uint64_t size, const std::string& workload, const std::string& path,
              const std::vector<std::uint64_t>& latencies_ns, const std::vector<std::uint64_t>& ops_per_rep,
              double mean_examined, const std::string& notes) {
    std::vector<double> per_op;
    double total_ns = 0, total_ops = 0;
    for (std::size_t i = 0; i < latencies_ns.size(); ++i) {
      per_op.push_back(static_cast<double>(latencies_ns[i]) / static_cast<double>(ops_per_rep[i]));
      total_ns += static_cast<double>(latencies_ns[i]);
      total_ops += static_cast<double>(ops_per_rep[i]);
      raw_ << run_id_ << "," << size << "," << workload << "," << path << "," << i << "," << ops_per_rep[i] << ","
           << latencies_ns[i] << "\n";
    }
    Summary s = summarize(per_op);
    s.mean = total_ops > 0 ? total_ns / total_ops : 0;  // exact mean per operation
    write_summary(size, workload, path, static_cast<std::uint64_t>(total_ops), s, mean_examined, notes);
  }

  void record_derived(std::uint64_t size, const std::string& workload, const std::string& path, double mean_ns,
                      std::uint64_t reps, const std::string& notes) {
    // Derived rows carry only a mean; the other statistics are left blank, not zero.
    summary_ << run_id_ << "," << size << "," << workload << "," << path << "," << reps << "," << mean_ns
             << ",,,,,,,," << (mean_ns > 0 ? 1e9 / mean_ns : 0) << ",," << csv_escape(notes) << "\n";
  }

  void flush() { summary_.flush(); raw_.flush(); }

 private:
  void write_summary(std::uint64_t size, const std::string& workload, const std::string& path, std::uint64_t reps,
                     const Summary& s, double examined, const std::string& notes) {
    summary_ << run_id_ << "," << size << "," << workload << "," << path << "," << reps << "," << s.mean << ","
             << s.median << "," << s.p90 << "," << s.p95 << "," << s.p99 << "," << s.min << "," << s.max << ","
             << s.stddev << "," << (s.mean > 0 ? 1e9 / s.mean : 0) << "," << examined << "," << csv_escape(notes) << "\n";
  }
  std::string run_id_;
  std::ofstream summary_, raw_;
};

Statement select_range(Key lo, Key hi) {
  Statement st;
  st.type = StatementType::Select;
  st.has_where = true;
  st.range.lo = lo;
  st.range.hi = hi;
  return st;
}

std::string cpu_model() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("model name", 0) == 0) {
      auto pos = line.find(':');
      return pos == std::string::npos ? line : line.substr(pos + 2);
    }
  }
  return "unknown";
}

unsigned cpu_count() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  unsigned n = 0;
  while (std::getline(f, line)) if (line.rfind("processor", 0) == 0) ++n;
  return n;
}

std::uint64_t mem_total_kb() {
  std::ifstream f("/proc/meminfo");
  std::string key;
  std::uint64_t v;
  std::string unit;
  while (f >> key >> v >> unit) if (key == "MemTotal:") return v;
  return 0;
}

class Bench {
 public:
  explicit Bench(const Config& c) : cfg_(c), rec_(c.out, c.run_id) {}

  void run() {
    started_ = utc_now();
    fs::create_directories(cfg_.workdir);
    for (std::uint64_t n : cfg_.sizes) run_size(n);
    finished_ = utc_now();
    rec_.flush();
    write_meta();
  }

 private:
  [[noreturn]] void fail(const std::string& m) { throw Failure{m}; }

  void run_size(std::uint64_t n) {
    emit_progress("start dataset", n, 0);
    // Disk space check: main file + one temporary file, ~110 bytes per record each.
    std::error_code ec;
    auto space = fs::space(cfg_.workdir, ec);
    std::uint64_t need = n * 110ULL * 2 + (64ULL << 20);
    if (!ec && space.available < need) {
      fail("not enough free disk space in " + cfg_.workdir + " for " + std::to_string(n) + " records (need ~" +
           std::to_string(need >> 20) + " MiB, have " + std::to_string(space.available >> 20) + " MiB)");
    }
    const std::vector<Key> keys = shuffled_keys(n, cfg_.seed);
    const std::string main_path = cfg_.workdir + "/bench_" + std::to_string(n) + ".dat";
    fs::remove(main_path, ec);

    // ---- A: heap only
    {
      emit_progress("insert_heap_only", n, 5);
      const std::string p = cfg_.workdir + "/heap_only_" + std::to_string(n) + ".dat";
      fs::remove(p, ec);
      HeapFile heap;
      check(heap.open(p, true), "open heap file");
      batch_insert(n, "insert_heap_only", "HEAP_ONLY", keys, "random key order; no index; no duplicate check",
                   [&](Key k, const std::string& payload) { return heap.insert(k, payload).status(); });
      heap.close();
      fs::remove(p, ec);
    }
    // ---- B: index only
    {
      emit_progress("insert_index_only", n, 15);
      BPlusTree tree(cfg_.max_keys);
      std::uint64_t i = 0;
      std::vector<std::uint64_t> lat, ops;
      for (std::size_t start = 0; start < keys.size(); start += 1000) {
        std::size_t end = std::min<std::size_t>(start + 1000, keys.size());
        auto t0 = Clock::now();
        for (std::size_t j = start; j < end; ++j, ++i) {
          if (!tree.insert(keys[j], RecordLocator{i + 1, kFileHeaderSize + i * 100})) fail("unexpected duplicate in index-only insert");
        }
        lat.push_back(ns_since(t0));
        ops.push_back(end - start);
      }
      rec_.record(n, "insert_index_only", "INDEX_ONLY", lat, ops, 0,
                  "random key order; in-memory B+ Tree only; max_keys=" + std::to_string(cfg_.max_keys));
      insert_index_only_mean_ = mean_per_op(lat, ops);
      if (!tree.validate().empty()) fail("B+ Tree invalid after index-only inserts");
    }
    // ---- C: full insert (main dataset)
    Database db;
    DatabaseOptions opt;
    opt.max_keys = cfg_.max_keys;
    {
      emit_progress("insert_full", n, 25);
      check(db.open(main_path, opt), "open main database");
      batch_insert(n, "insert_full", "HEAP_PLUS_INDEX", keys, "random key order; duplicate probe + heap append + index insert",
                   [&](Key k, const std::string& payload) { return db.insert(k, payload); });
      if (heap_only_mean_ > 0) {
        rec_.record_derived(n, "index_maintenance_overhead", "DERIVED", insert_full_mean_ - heap_only_mean_, n,
                            "derived: insert_full mean minus insert_heap_only mean per record (index probe + index insert)");
      }
      ConsistencyReport rep = db.check_consistency();
      if (!rep.consistent) fail("heap/index inconsistent after bulk insert: " + (rep.problems.empty() ? std::string("?") : rep.problems[0]));
    }
    // ---- D: ascending insert
    {
      emit_progress("insert_full_ascending", n, 40);
      const std::string p = cfg_.workdir + "/ascending_" + std::to_string(n) + ".dat";
      fs::remove(p, ec);
      Database asc;
      check(asc.open(p, opt), "open ascending database");
      std::vector<Key> ascending(n);
      std::iota(ascending.begin(), ascending.end(), 1);
      batch_insert(n, "insert_full_ascending", "HEAP_PLUS_INDEX", ascending, "ascending key order into a fresh database",
                   [&](Key k, const std::string& payload) { return asc.insert(k, payload); });
      asc.close();
      fs::remove(p, ec);
    }
    // ---- E: reopen = validate file + rebuild index
    {
      emit_progress("index_rebuild_on_open", n, 50);
      std::vector<std::uint64_t> lat, ops;
      for (int r = 0; r < 3; ++r) {
        db.close();
        auto t0 = Clock::now();
        check(db.open(main_path, opt), "reopen main database");
        lat.push_back(ns_since(t0));
        ops.push_back(1);
      }
      rec_.record(n, "index_rebuild_on_open", "HEAP_SCAN_PLUS_INDEX_BUILD", lat, ops, static_cast<double>(n),
                  "close + open: CRC-validate every slot and rebuild the B+ Tree; 3 repetitions");
      ConsistencyReport rep = db.check_consistency();
      if (!rep.consistent) fail("heap/index inconsistent after reopen");
    }

    run_queries(db, n);
    db.close();
    if (!keep_files_) fs::remove(main_path, ec);
    emit_progress("dataset complete", n, 100);
  }

  void run_queries(Database& db, std::uint64_t n) {
    QueryEngine engine(db);
    std::mt19937_64 rng(cfg_.seed + 7919);
    ExecOptions opts;
    opts.max_rows = 0;  // unlimited: both paths materialise every matching row

    // point lookups
    for (ForcePath fp : {ForcePath::Index, ForcePath::Scan}) {
      const bool idx = fp == ForcePath::Index;
      emit_progress(idx ? "point_lookup (index)" : "point_lookup (scan)", n, idx ? 55 : 60);
      const std::size_t reps = idx ? cfg_.reps : cfg_.scan_reps;
      auto make = [&]() { Key k = static_cast<Key>(1 + rng() % n); return std::make_pair(select_range(k, k), std::uint64_t{1}); };
      auto auto_plan = engine.plan(make().first, ForcePath::Auto);
      measure_query(engine, n, "point_lookup", idx ? "INDEX_POINT" : "FULL_SCAN", fp, reps, opts, make,
                    "existing key; auto_path=" + std::string(access_path_name(auto_plan.value().path)));
    }
    // full scan: predicate that matches nothing (keys are 1..n), so every record is visited
    {
      emit_progress("full_scan", n, 65);
      auto make = [&]() { return std::make_pair(select_range(0, 0), std::uint64_t{0}); };
      measure_query(engine, n, "full_scan", "FULL_SCAN", ForcePath::Scan, cfg_.scan_reps, opts, make,
                    "predicate matches no record; visits all records; expected_examined=" + std::to_string(n));
    }
    // ranges
    struct R { const char* name; double frac; };
    const R ranges[] = {{"range_0.1pct", 0.001}, {"range_1pct", 0.01}, {"range_10pct", 0.10}, {"range_50pct", 0.50}};
    double pct = 68;
    for (const R& r : ranges) {
      const std::uint64_t width = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(static_cast<double>(n) * r.frac)));
      const bool heavy = r.frac >= 0.10;
      auto make = [&]() {
        Key lo = static_cast<Key>(1 + rng() % (n - width + 1));
        return std::make_pair(select_range(lo, lo + static_cast<Key>(width) - 1), width);
      };
      auto auto_plan = engine.plan(select_range(1, static_cast<Key>(width)), ForcePath::Auto);
      const std::string note = "width=" + std::to_string(width) + " keys; keys are dense 1..N so matches=width; auto_path=" +
                               access_path_name(auto_plan.value().path);
      emit_progress(std::string(r.name) + " (index)", n, pct);
      measure_query(engine, n, r.name, "INDEX_RANGE", ForcePath::Index, heavy ? cfg_.heavy_reps : cfg_.range_reps, opts, make, note);
      emit_progress(std::string(r.name) + " (scan)", n, pct + 3);
      measure_query(engine, n, r.name, "FULL_SCAN", ForcePath::Scan, heavy ? cfg_.heavy_reps : cfg_.scan_reps, opts, make, note);
      pct += 7;
    }
  }

  // `make()` returns {statement, expected row count}; the result is verified every repetition.
  template <class Make>
  void measure_query(QueryEngine& engine, std::uint64_t n, const std::string& workload, const std::string& path_label,
                     ForcePath fp, std::size_t reps, const ExecOptions& opts, Make&& make, const std::string& notes) {
    std::vector<std::uint64_t> lat, ops;
    double examined = 0;
    for (std::size_t i = 0; i < cfg_.warmup + reps; ++i) {
      auto [st, expected] = make();
      Result<Plan> plan = engine.plan(st, fp);
      if (!plan.ok()) fail("planning failed: " + plan.status().to_string());
      auto t0 = Clock::now();
      QueryResult r = engine.execute_plan(plan.value(), opts);
      std::uint64_t ns = ns_since(t0);
      if (!r.ok()) fail(workload + ": query failed: " + r.status.to_string());
      if (r.rows.size() != expected) {
        fail(workload + " (" + path_label + "): expected " + std::to_string(expected) + " rows, got " + std::to_string(r.rows.size()));
      }
      if (i >= cfg_.warmup) {
        lat.push_back(ns);
        ops.push_back(1);
        examined += static_cast<double>(r.records_examined);
      }
    }
    (void)n;
    rec_.record(n, workload, path_label, lat, ops, reps ? examined / static_cast<double>(reps) : 0, notes);
  }

  template <class InsertFn>
  void batch_insert(std::uint64_t n, const std::string& workload, const std::string& path_label, const std::vector<Key>& keys,
                    const std::string& notes, InsertFn&& insert) {
    std::vector<std::uint64_t> lat, ops;
    std::vector<std::string> payloads;
    for (std::size_t start = 0; start < keys.size(); start += 1000) {
      std::size_t end = std::min<std::size_t>(start + 1000, keys.size());
      payloads.clear();
      for (std::size_t j = start; j < end; ++j) payloads.push_back(make_payload(cfg_.seed, keys[j]));
      auto t0 = Clock::now();
      for (std::size_t j = start; j < end; ++j) {
        Status st = insert(keys[j], payloads[j - start]);
        if (!st.ok()) fail(workload + ": insert failed: " + st.to_string());
      }
      lat.push_back(ns_since(t0));
      ops.push_back(end - start);
    }
    rec_.record(n, workload, path_label, lat, ops, 0, notes + "; percentiles are over 1000-insert batches");
    double mean = mean_per_op(lat, ops);
    if (workload == "insert_heap_only") heap_only_mean_ = mean;
    if (workload == "insert_full") insert_full_mean_ = mean;
  }

  static double mean_per_op(const std::vector<std::uint64_t>& lat, const std::vector<std::uint64_t>& ops) {
    double t = 0, o = 0;
    for (std::size_t i = 0; i < lat.size(); ++i) { t += static_cast<double>(lat[i]); o += static_cast<double>(ops[i]); }
    return o > 0 ? t / o : 0;
  }
  static std::uint64_t ns_since(Clock::time_point t0) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
  }
  void check(const Status& s, const std::string& what) {
    if (!s.ok()) fail(what + ": " + s.to_string());
  }

  void write_meta() {
    utsname u{};
    uname(&u);
    std::ostringstream os;
    os << "{\n  \"run_id\": " << json_quote(cfg_.run_id) << ",\n  \"started_utc\": " << json_quote(started_)
       << ",\n  \"finished_utc\": " << json_quote(finished_) << ",\n  \"dataset_sizes\": [";
    for (std::size_t i = 0; i < cfg_.sizes.size(); ++i) os << (i ? "," : "") << cfg_.sizes[i];
    os << "],\n  \"parameters\": {\"point_lookup_index_reps\": " << cfg_.reps << ", \"scan_reps\": " << cfg_.scan_reps
       << ", \"range_index_reps\": " << cfg_.range_reps << ", \"heavy_range_reps\": " << cfg_.heavy_reps
       << ", \"warmup_runs_per_measurement\": " << cfg_.warmup << ", \"btree_max_keys\": " << cfg_.max_keys
       << ", \"seed\": " << cfg_.seed << "},\n  \"system\": {\"cpu_model\": " << json_quote(cpu_model())
       << ", \"logical_cpus\": " << cpu_count() << ", \"ram_total_mib\": " << mem_total_kb() / 1024
       << ", \"os\": " << json_quote(std::string(u.sysname) + " " + u.release + " " + u.machine) << "},\n  \"build\": "
       << build_info_json() << ",\n  \"methodology\": {"
       << "\"key_distribution\": \"keys are a seeded random permutation of 1..N (unique, dense), inserted in random order\", "
       << "\"payload\": \"deterministic pseudo-random printable text, 16-111 bytes (mean ~64)\", "
       << "\"timing_boundary\": \"steady_clock around QueryEngine::execute_plan with a prepared plan (parse and plan excluded); rows are materialised; inserts timed in batches of 1000 with payload generation excluded\", "
       << "\"cache_conditions\": \"warm OS page cache; caches were not dropped between runs\", "
       << "\"warmup\": \"each measurement runs warmup_runs_per_measurement untimed executions first\", "
       << "\"verification\": \"every query result is checked against the expected row count; heap/index consistency is checked after loading\""
       << "}\n}\n";
    std::ofstream(cfg_.out + "/meta.json") << os.str();
  }

  Config cfg_;
  Recorder rec_;
  std::string started_, finished_;
  double heap_only_mean_ = 0, insert_full_mean_ = 0, insert_index_only_mean_ = 0;
  bool keep_files_ = false;
};

bool parse_sizes(const std::string& s, std::vector<std::uint64_t>& out) {
  out.clear();
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (tok.empty()) return false;
    char* end = nullptr;
    unsigned long long v = std::strtoull(tok.c_str(), &end, 10);
    if (*end != '\0' || v < 100 || v > 50000000ULL) return false;
    out.push_back(v);
  }
  return !out.empty();
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](std::string& v) {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); return false; }
      v = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--sizes") { if (!next(v) || !parse_sizes(v, cfg.sizes)) { std::fprintf(stderr, "bad --sizes (comma-separated integers 100..50000000)\n"); return 2; } }
    else if (a == "--reps") { if (!next(v)) return 2; cfg.reps = std::strtoull(v.c_str(), nullptr, 10); }
    else if (a == "--scan-reps") { if (!next(v)) return 2; cfg.scan_reps = std::strtoull(v.c_str(), nullptr, 10); }
    else if (a == "--range-reps") { if (!next(v)) return 2; cfg.range_reps = std::strtoull(v.c_str(), nullptr, 10); }
    else if (a == "--heavy-reps") { if (!next(v)) return 2; cfg.heavy_reps = std::strtoull(v.c_str(), nullptr, 10); }
    else if (a == "--warmup") { if (!next(v)) return 2; cfg.warmup = std::strtoull(v.c_str(), nullptr, 10); }
    else if (a == "--max-keys") { if (!next(v)) return 2; cfg.max_keys = std::strtoull(v.c_str(), nullptr, 10); }
    else if (a == "--seed") { if (!next(v)) return 2; cfg.seed = std::strtoull(v.c_str(), nullptr, 10); }
    else if (a == "--workdir") { if (!next(cfg.workdir)) return 2; }
    else if (a == "--out") { if (!next(cfg.out)) return 2; }
    else if (a == "--run-id") { if (!next(cfg.run_id)) return 2; }
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  if (cfg.max_keys < 3 || cfg.reps == 0 || cfg.scan_reps == 0 || cfg.range_reps == 0 || cfg.heavy_reps == 0) {
    std::fprintf(stderr, "invalid parameters\n");
    return 2;
  }
  if (cfg.run_id.empty()) cfg.run_id = "run-" + std::to_string(std::time(nullptr));
  try {
    Bench b(cfg);
    b.run();
    std::printf("{\"event\":\"done\",\"summary\":%s,\"raw\":%s,\"meta\":%s}\n", json_quote(cfg.out + "/summary.csv").c_str(),
                json_quote(cfg.out + "/raw.csv").c_str(), json_quote(cfg.out + "/meta.json").c_str());
    return 0;
  } catch (const Failure& f) {
    std::printf("{\"event\":\"error\",\"message\":%s}\n", json_quote(f.message).c_str());
    return 1;
  } catch (const std::exception& e) {
    std::printf("{\"event\":\"error\",\"message\":%s}\n", json_quote(e.what()).c_str());
    return 1;
  }
}
