#include "indexcraft/c_api.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <sstream>

#include "indexcraft/build_info.hpp"
#include "indexcraft/database.hpp"
#include "indexcraft/json.hpp"
#include "indexcraft/query.hpp"

using namespace indexcraft;

struct ic_handle {
  Database db;
  QueryEngine engine;
  ic_handle() : engine(db) {}
};

namespace {

char* dup_string(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (!p) return nullptr;
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

std::string error_json(const Status& st) {
  return "{\"ok\":false,\"error\":{\"code\":" + json_quote(code_name(st.code())) +
         ",\"message\":" + json_quote(st.message()) + "}}";
}

int fail(const Status& st, char** out) {
  if (out) *out = dup_string(error_json(st));
  return static_cast<int>(st.code());
}

int succeed(const std::string& json, char** out) {
  if (out) *out = dup_string(json);
  return 0;
}

// Runs `fn`, converting exceptions into an INTERNAL error so none cross the C boundary.
template <typename F>
int guarded(char** out, F&& fn) {
  try {
    return fn();
  } catch (const std::exception& e) {
    return fail(Status::Error(Code::Internal, std::string("unexpected exception: ") + e.what()), out);
  } catch (...) {
    return fail(Status::Error(Code::Internal, "unexpected unknown exception"), out);
  }
}

bool bad_handle(ic_handle* h, char** out, int* rc) {
  if (!h || !h->db.is_open()) {
    *rc = fail(Status::Error(Code::InvalidArgument, "database handle is not open"), out);
    return true;
  }
  return false;
}

std::string base_name(const std::string& path) {
  auto pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

}  // namespace

extern "C" {

int ic_open(const char* path, size_t max_keys, ic_handle** out_handle, char** out_json) {
  return guarded(out_json, [&]() {
    if (!path || !out_handle) return fail(Status::Error(Code::InvalidArgument, "null argument"), out_json);
    auto h = std::make_unique<ic_handle>();
    DatabaseOptions opt;
    if (max_keys) opt.max_keys = max_keys;
    Status st = h->db.open(path, opt);
    if (!st.ok()) return fail(st, out_json);
    *out_handle = h.release();
    return succeed("{\"ok\":true}", out_json);
  });
}

void ic_close(ic_handle* h) {
  if (!h) return;
  h->db.close();
  delete h;
}

void ic_free(char* s) { std::free(s); }

int ic_execute(ic_handle* h, const char* sql, int force, size_t max_rows, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    if (!sql) return fail(Status::Error(Code::InvalidArgument, "null query"), out_json);
    ExecOptions opt;
    opt.force = force == 1 ? ForcePath::Scan : force == 2 ? ForcePath::Index : ForcePath::Auto;
    opt.max_rows = max_rows;
    QueryResult r = h->engine.execute(sql, opt);
    *out_json = dup_string(to_json(r));
    return r.ok() ? 0 : static_cast<int>(r.status.code());
  });
}

int ic_compare(ic_handle* h, const char* sql, size_t max_rows, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    if (!sql) return fail(Status::Error(Code::InvalidArgument, "null query"), out_json);
    ExecOptions opt;
    opt.max_rows = max_rows;
    CompareResult r = h->engine.compare(sql, opt);
    *out_json = dup_string(to_json(r));
    return r.ok() ? 0 : static_cast<int>(r.status.code());
  });
}

int ic_insert(ic_handle* h, int64_t key, const char* payload, size_t payload_len, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    std::string data = payload ? std::string(payload, payload_len) : std::string();
    RecordLocator loc;
    Status st = h->db.insert(key, data, &loc);
    if (!st.ok()) return fail(st, out_json);
    std::ostringstream os;
    os << "{\"ok\":true,\"key\":" << key << ",\"rid\":" << loc.rid << ",\"offset\":" << loc.offset << "}";
    return succeed(os.str(), out_json);
  });
}

int ic_delete(ic_handle* h, int64_t key, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    Status st = h->db.erase(key);
    if (!st.ok()) return fail(st, out_json);
    return succeed("{\"ok\":true,\"key\":" + std::to_string(key) + "}", out_json);
  });
}

int ic_get(ic_handle* h, int64_t key, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    Result<Record> r = h->db.get(key);
    if (!r.ok()) return fail(r.status(), out_json);
    std::ostringstream os;
    os << "{\"ok\":true,\"record\":{\"key\":" << r.value().key << ",\"rid\":" << r.value().rid
       << ",\"payload\":" << json_quote(r.value().payload) << "}}";
    return succeed(os.str(), out_json);
  });
}

int ic_list(ic_handle* h, size_t offset, size_t limit, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    Result<std::vector<Record>> r = h->db.list(offset, limit);
    if (!r.ok()) return fail(r.status(), out_json);
    std::ostringstream os;
    os << "{\"ok\":true,\"total\":" << h->db.stats().record_count << ",\"offset\":" << offset << ",\"records\":[";
    for (std::size_t i = 0; i < r.value().size(); ++i) {
      const Record& rec = r.value()[i];
      os << (i ? "," : "") << "{\"key\":" << rec.key << ",\"rid\":" << rec.rid << ",\"payload\":" << json_quote(rec.payload)
         << "}";
    }
    os << "]}";
    return succeed(os.str(), out_json);
  });
}

int ic_stats(ic_handle* h, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    DatabaseStats s = h->db.stats();
    const auto& c = h->db.index().counters();
    std::ostringstream os;
    os << "{\"ok\":true,\"data_file\":" << json_quote(base_name(h->db.heap().path()))
       << ",\"record_count\":" << s.record_count << ",\"file_size_bytes\":" << s.file_size_bytes
       << ",\"deleted_slots\":" << s.deleted_slots << ",\"next_record_id\":" << s.next_rid
       << ",\"index_height\":" << s.index_height << ",\"index_key_count\":" << s.index_key_count
       << ",\"index_node_count\":" << s.index_node_count << ",\"index_leaf_count\":" << s.index_leaf_count
       << ",\"index_internal_count\":" << s.index_internal_count << ",\"index_max_keys\":" << s.index_max_keys
       << ",\"open_rebuild_ms\":" << static_cast<double>(s.open_ns) / 1e6
       << ",\"index_events\":{\"leaf_splits\":" << c.leaf_splits << ",\"internal_splits\":" << c.internal_splits
       << ",\"root_splits\":" << c.root_splits << ",\"leaf_redistributions\":" << c.leaf_redistributions
       << ",\"internal_redistributions\":" << c.internal_redistributions << ",\"leaf_merges\":" << c.leaf_merges
       << ",\"internal_merges\":" << c.internal_merges << ",\"root_shrinks\":" << c.root_shrinks << "}}";
    return succeed(os.str(), out_json);
  });
}

int ic_tree(ic_handle* h, size_t max_nodes, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    std::string tree = h->db.index().to_json(max_nodes ? max_nodes : 5000);
    // splice "ok":true into the object produced by the tree
    return succeed("{\"ok\":true," + tree.substr(1), out_json);
  });
}

int ic_search_path(ic_handle* h, int64_t key, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    BPlusTree::SearchPath p = h->db.index().search_path(key);
    std::ostringstream os;
    os << "{\"ok\":true,\"key\":" << key << ",\"found\":" << (p.found ? "true" : "false") << ",\"node_ids\":[";
    for (std::size_t i = 0; i < p.node_ids.size(); ++i) os << (i ? "," : "") << p.node_ids[i];
    os << "]}";
    return succeed(os.str(), out_json);
  });
}

int ic_validate(ic_handle* h, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    std::vector<std::string> v = h->db.index().validate();
    std::ostringstream os;
    os << "{\"ok\":true,\"valid\":" << (v.empty() ? "true" : "false") << ",\"violations\":[";
    for (std::size_t i = 0; i < v.size() && i < 50; ++i) os << (i ? "," : "") << json_quote(v[i]);
    os << "],\"height\":" << h->db.index().height() << ",\"key_count\":" << h->db.index().size()
       << ",\"node_count\":" << h->db.index().node_count() << "}";
    return succeed(os.str(), out_json);
  });
}

int ic_check_consistency(ic_handle* h, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    ConsistencyReport r = h->db.check_consistency();
    std::ostringstream os;
    os << "{\"ok\":true,\"consistent\":" << (r.consistent ? "true" : "false")
       << ",\"heap_live_records\":" << r.heap_live_records << ",\"index_entries\":" << r.index_entries
       << ",\"records_checked\":" << r.records_checked << ",\"problems\":[";
    for (std::size_t i = 0; i < r.problems.size(); ++i) os << (i ? "," : "") << json_quote(r.problems[i]);
    os << "]}";
    return succeed(os.str(), out_json);
  });
}

int ic_rebuild_index(ic_handle* h, char** out_json) {
  return guarded(out_json, [&]() {
    int rc;
    if (bad_handle(h, out_json, &rc)) return rc;
    Status st = h->db.rebuild_index();
    if (!st.ok()) return fail(st, out_json);
    return succeed("{\"ok\":true,\"index_key_count\":" + std::to_string(h->db.index().size()) + "}", out_json);
  });
}

int ic_build_info(char** out_json) {
  return guarded(out_json, [&]() {
    std::string info = build_info_json();
    return succeed("{\"ok\":true," + info.substr(1), out_json);
  });
}

}  // extern "C"
