// indexcraft_cli: initialise a data store, generate sample records, run queries, check integrity.
//
//   indexcraft_cli init     --db FILE [--max-keys N]
//   indexcraft_cli generate --db FILE --count N [--seed S] [--start-key K]
//   indexcraft_cli query    --db FILE "SQL" [--path auto|scan|index] [--max-rows N]
//   indexcraft_cli compare  --db FILE "SELECT ..." [--max-rows N]
//   indexcraft_cli stats    --db FILE
//   indexcraft_cli check    --db FILE
//   indexcraft_cli info
//
// Every command prints one JSON document on stdout. Exit code 0 = success.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "indexcraft/build_info.hpp"
#include "indexcraft/c_api.h"

namespace {

struct Args {
  std::string command;
  std::string db;
  std::string positional;
  std::string path = "auto";
  std::size_t max_keys = 64;
  std::size_t max_rows = 1000;
  std::uint64_t count = 0;
  std::uint64_t seed = 1;
  std::int64_t start_key = 1;
};

bool parse_args(int argc, char** argv, Args& a, std::string& err) {
  if (argc < 2) { err = "missing command"; return false; }
  a.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string s = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) { err = "missing value for " + s; return false; }
      out = argv[++i];
      return true;
    };
    std::string v;
    if (s == "--db") { if (!value(a.db)) return false; }
    else if (s == "--path") { if (!value(a.path)) return false; }
    else if (s == "--max-keys") { if (!value(v)) return false; a.max_keys = std::strtoull(v.c_str(), nullptr, 10); }
    else if (s == "--max-rows") { if (!value(v)) return false; a.max_rows = std::strtoull(v.c_str(), nullptr, 10); }
    else if (s == "--count") { if (!value(v)) return false; a.count = std::strtoull(v.c_str(), nullptr, 10); }
    else if (s == "--seed") { if (!value(v)) return false; a.seed = std::strtoull(v.c_str(), nullptr, 10); }
    else if (s == "--start-key") { if (!value(v)) return false; a.start_key = std::strtoll(v.c_str(), nullptr, 10); }
    else if (!s.empty() && s[0] == '-' && s.size() > 1 && !std::isdigit(static_cast<unsigned char>(s[1]))) { err = "unknown option " + s; return false; }
    else a.positional = s;
  }
  return true;
}

int print_and_free(int rc, char* json) {
  if (json) {
    std::cout << json << "\n";
    ic_free(json);
  }
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  std::string err;
  if (!parse_args(argc, argv, a, err)) {
    std::cerr << "error: " << err << "\nusage: indexcraft_cli <init|generate|query|compare|stats|check|info> --db FILE ...\n";
    return 2;
  }
  if (a.command == "info") {
    char* out = nullptr;
    int rc = ic_build_info(&out);
    return print_and_free(rc, out);
  }
  if (a.db.empty()) {
    std::cerr << "error: --db FILE is required\n";
    return 2;
  }

  ic_handle* h = nullptr;
  char* out = nullptr;
  int rc = ic_open(a.db.c_str(), a.max_keys, &h, &out);
  if (rc != 0) return print_and_free(rc, out);
  ic_free(out);
  out = nullptr;

  if (a.command == "init") {
    rc = ic_stats(h, &out);
  } else if (a.command == "generate") {
    std::mt19937_64 rng(a.seed);
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789 ";
    std::uint64_t inserted = 0, skipped = 0;
    for (std::uint64_t i = 0; i < a.count; ++i) {
      std::int64_t key = a.start_key + static_cast<std::int64_t>(i);
      std::string payload = "sample-" + std::to_string(key) + ": ";
      std::size_t extra = 8 + rng() % 40;
      for (std::size_t k = 0; k < extra; ++k) payload.push_back(kAlphabet[rng() % (sizeof(kAlphabet) - 1)]);
      char* res = nullptr;
      int r = ic_insert(h, key, payload.data(), payload.size(), &res);
      ic_free(res);
      if (r == 0) ++inserted; else ++skipped;
    }
    ic_free(out);
    out = nullptr;
    rc = ic_stats(h, &out);
    std::cerr << "generated: inserted " << inserted << ", skipped (duplicate keys) " << skipped << "\n";
  } else if (a.command == "query") {
    if (a.positional.empty()) { std::cerr << "error: missing SQL text\n"; ic_close(h); return 2; }
    int force = a.path == "scan" ? 1 : a.path == "index" ? 2 : 0;
    rc = ic_execute(h, a.positional.c_str(), force, a.max_rows, &out);
  } else if (a.command == "compare") {
    if (a.positional.empty()) { std::cerr << "error: missing SQL text\n"; ic_close(h); return 2; }
    rc = ic_compare(h, a.positional.c_str(), a.max_rows, &out);
  } else if (a.command == "stats") {
    rc = ic_stats(h, &out);
  } else if (a.command == "check") {
    char* cons = nullptr;
    char* tree = nullptr;
    int r1 = ic_check_consistency(h, &cons);
    int r2 = ic_validate(h, &tree);
    std::cout << "{\"consistency\":" << (cons ? cons : "null") << ",\"tree\":" << (tree ? tree : "null") << "}\n";
    bool bad = std::string(cons ? cons : "").find("\"consistent\":false") != std::string::npos ||
               std::string(tree ? tree : "").find("\"valid\":false") != std::string::npos;
    ic_free(cons);
    ic_free(tree);
    ic_close(h);
    return (r1 != 0 || r2 != 0 || bad) ? 1 : 0;
  } else {
    std::cerr << "error: unknown command '" << a.command << "'\n";
    ic_close(h);
    return 2;
  }
  int result = print_and_free(rc, out);
  ic_close(h);
  return result;
}
