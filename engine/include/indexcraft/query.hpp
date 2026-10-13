#pragma once
// Query layer: tokenizer, parser, planner and executor for a small SQL-like subset.
//
// GRAMMAR (keywords are case-insensitive; one statement per request; trailing ';' optional)
//
//   INSERT INTO records (key, payload) VALUES (<integer>, '<text>')     -- columns may be in either order
//   SELECT * FROM records [WHERE <cond> {AND <cond>}] [LIMIT <n>]
//   DELETE FROM records WHERE key = <integer>
//   EXPLAIN <INSERT|SELECT|DELETE ...>                                   -- plan only, nothing executed
//
//   <cond> := key (=|<|<=|>|>=) <integer>  |  key BETWEEN <integer> AND <integer>
//
// Strings use '...' or "..."; a quote is escaped by doubling it ('it''s'). Integers are
// signed 64-bit. Only column `key` can be filtered. OR, joins, projections, UPDATE and
// range DELETE are not supported and produce an UNSUPPORTED / SYNTAX_ERROR status.
//
// SELECT results are ALWAYS ordered by key ascending, and LIMIT keeps the first n rows in
// that order, so the scan path and the index path return identical results.
//
// ACCESS-PATH SELECTION (ForcePath::Auto)
//   * equality on key                          -> INDEX_POINT
//   * range on key                             -> INDEX_RANGE if the estimated fraction of
//                                                 records in the range is <= 25%, else FULL_SCAN
//                                                 (many random record fetches cost more than one
//                                                 sequential pass)
//   * no WHERE clause                          -> FULL_SCAN
//   The range estimate assumes keys are spread uniformly between the tree's min and max key.
//   INSERT and DELETE always use the index (duplicate probe / locate), reported as
//   INDEX_INSERT / INDEX_POINT.

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "indexcraft/database.hpp"
#include "indexcraft/status.hpp"

namespace indexcraft {

constexpr double kScanSelectivityThreshold = 0.25;

struct Token {
  enum class Kind { Ident, Integer, String, Symbol, End };
  Kind kind = Kind::End;
  std::string text;          // Ident: as written; String: unescaped content; Symbol: the symbol
  std::int64_t int_value = 0;
  std::size_t pos = 0;       // character offset in the query text
};

Result<std::vector<Token>> tokenize(const std::string& sql);

enum class StatementType { Insert, Select, Delete };
enum class AccessPath { None, FullScan, IndexPoint, IndexRange, IndexInsert };
enum class ForcePath { Auto, Scan, Index };

const char* statement_type_name(StatementType t);
const char* access_path_name(AccessPath p);

// Inclusive key interval [lo, hi]; `empty` is set for contradictory predicates.
struct KeyRange {
  Key lo = std::numeric_limits<Key>::min();
  Key hi = std::numeric_limits<Key>::max();
  bool empty = false;
  bool is_point() const { return !empty && lo == hi; }
  bool is_unbounded() const {
    return !empty && lo == std::numeric_limits<Key>::min() && hi == std::numeric_limits<Key>::max();
  }
};

struct Statement {
  StatementType type = StatementType::Select;
  bool explain = false;
  Key key = 0;               // INSERT
  std::string payload;       // INSERT
  bool has_where = false;    // SELECT / DELETE
  KeyRange range;            // SELECT / DELETE
  std::optional<std::uint64_t> limit;
};

Result<Statement> parse(const std::string& sql);

struct Plan {
  Statement stmt;
  AccessPath path = AccessPath::None;
  bool forced = false;
  double estimated_matches = 0;
  double estimated_selectivity = 0;
  std::vector<std::string> explain;
};

struct ExecOptions {
  ForcePath force = ForcePath::Auto;
  std::size_t max_rows = 1000;  // cap on returned SELECT rows; 0 = unlimited
};

struct Row {
  Key key = 0;
  RecordId rid = 0;
  std::string payload;
};

struct QueryResult {
  Status status;
  StatementType type = StatementType::Select;
  bool type_known = true;          // false when the query failed to parse
  AccessPath path = AccessPath::None;
  bool path_forced = false;
  bool executed = false;           // false for EXPLAIN or when parsing/planning failed
  std::vector<Row> rows;
  std::uint64_t rows_matched = 0;  // matches counted (scan: exact; index with a cap: may stop early)
  bool has_more = false;           // more matching rows exist beyond those returned
  std::uint64_t rows_affected = 0; // INSERT / DELETE
  std::uint64_t records_examined = 0;      // scan: live records visited; index: records fetched
  std::uint64_t index_lookups = 0;         // B+ Tree descents
  std::uint64_t index_nodes_visited = 0;   // tree nodes touched by those descents
  std::uint64_t heap_reads = 0;            // pread calls issued for point reads
  std::uint64_t parse_ns = 0;
  std::uint64_t exec_ns = 0;               // planning + execution, monotonic clock
  std::vector<std::string> plan;
  std::string message;
  bool ok() const { return status.ok(); }
};

struct CompareResult {
  Status status;
  QueryResult scan;
  QueryResult index;
  bool results_match = false;
  std::string mismatch;            // empty when results match
  double speedup = 0;              // scan exec_ns / index exec_ns
  bool ok() const { return status.ok(); }
};

class QueryEngine {
 public:
  explicit QueryEngine(Database& db) : db_(db) {}

  QueryResult execute(const std::string& sql, const ExecOptions& opts = {});
  Result<Plan> plan(const Statement& stmt, ForcePath force) const;
  // Executes a prepared plan. Used directly by the benchmark so parsing is excluded from timing.
  QueryResult execute_plan(const Plan& plan, const ExecOptions& opts = {});
  // Runs the same SELECT through FULL_SCAN and through the index and compares the rows.
  CompareResult compare(const std::string& sql, const ExecOptions& opts = {});

 private:
  QueryResult exec_select(const Plan& plan, const ExecOptions& opts);
  QueryResult exec_insert(const Plan& plan);
  QueryResult exec_delete(const Plan& plan);
  Database& db_;
};

std::string to_json(const QueryResult& r);
std::string to_json(const CompareResult& r);

}  // namespace indexcraft
