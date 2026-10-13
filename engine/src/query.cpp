#include "indexcraft/query.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "indexcraft/json.hpp"

namespace indexcraft {

namespace {

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

Status syntax_error(std::size_t pos, const std::string& msg) {
  return Status::Error(Code::SyntaxError, msg + " (at position " + std::to_string(pos) + ")");
}

std::string describe(const Token& t) {
  switch (t.kind) {
    case Token::Kind::End: return "end of input";
    case Token::Kind::String: return "string '" + t.text + "'";
    default: return "'" + t.text + "'";
  }
}

}  // namespace

const char* statement_type_name(StatementType t) {
  switch (t) {
    case StatementType::Insert: return "INSERT";
    case StatementType::Select: return "SELECT";
    case StatementType::Delete: return "DELETE";
  }
  return "UNKNOWN";
}

const char* access_path_name(AccessPath p) {
  switch (p) {
    case AccessPath::None: return "NONE";
    case AccessPath::FullScan: return "FULL_SCAN";
    case AccessPath::IndexPoint: return "INDEX_POINT";
    case AccessPath::IndexRange: return "INDEX_RANGE";
    case AccessPath::IndexInsert: return "INDEX_INSERT";
  }
  return "UNKNOWN";
}

// ================================================================ tokenizer

Result<std::vector<Token>> tokenize(const std::string& sql) {
  std::vector<Token> out;
  const std::size_t n = sql.size();
  std::size_t i = 0;
  while (i < n) {
    const char c = sql[i];
    const auto uc = static_cast<unsigned char>(c);
    if (std::isspace(uc)) { ++i; continue; }

    Token t;
    t.pos = i;
    if (std::isalpha(uc) || c == '_') {
      std::size_t j = i;
      while (j < n && (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_')) ++j;
      t.kind = Token::Kind::Ident;
      t.text = sql.substr(i, j - i);
      i = j;
    } else if (std::isdigit(uc) || (c == '-' && i + 1 < n && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
      std::size_t j = i + (c == '-' ? 1 : 0);
      while (j < n && std::isdigit(static_cast<unsigned char>(sql[j]))) ++j;
      if (j < n && sql[j] == '.') return syntax_error(j, "decimal numbers are not supported; keys are integers");
      if (j < n && (std::isalpha(static_cast<unsigned char>(sql[j])) || sql[j] == '_')) {
        return syntax_error(i, "invalid number '" + sql.substr(i, j - i + 1) + "...'");
      }
      std::string text = sql.substr(i, j - i);
      errno = 0;
      char* end = nullptr;
      long long v = std::strtoll(text.c_str(), &end, 10);
      if (errno == ERANGE) return syntax_error(i, "integer " + text + " is outside the signed 64-bit range");
      t.kind = Token::Kind::Integer;
      t.text = text;
      t.int_value = v;
      i = j;
    } else if (c == '\'' || c == '"') {
      const char quote = c;
      std::string value;
      std::size_t j = i + 1;
      bool closed = false;
      while (j < n) {
        if (sql[j] == quote) {
          if (j + 1 < n && sql[j + 1] == quote) { value.push_back(quote); j += 2; continue; }
          closed = true;
          ++j;
          break;
        }
        value.push_back(sql[j]);
        ++j;
      }
      if (!closed) return syntax_error(i, "unterminated string literal");
      t.kind = Token::Kind::String;
      t.text = value;
      i = j;
    } else {
      t.kind = Token::Kind::Symbol;
      switch (c) {
        case '(': case ')': case ',': case ';': case '*': case '=':
          t.text = std::string(1, c);
          ++i;
          break;
        case '<':
          if (i + 1 < n && sql[i + 1] == '=') { t.text = "<="; i += 2; }
          else if (i + 1 < n && sql[i + 1] == '>') return Status::Error(Code::Unsupported, "operator '<>' is not supported (at position " + std::to_string(i) + ")");
          else { t.text = "<"; ++i; }
          break;
        case '>':
          if (i + 1 < n && sql[i + 1] == '=') { t.text = ">="; i += 2; }
          else { t.text = ">"; ++i; }
          break;
        case '!':
          return Status::Error(Code::Unsupported, "operator '!=' is not supported (at position " + std::to_string(i) + ")");
        default:
          return syntax_error(i, std::string("unexpected character '") + c + "'");
      }
    }
    out.push_back(std::move(t));
  }
  Token end;
  end.kind = Token::Kind::End;
  end.pos = n;
  out.push_back(end);
  return out;
}

// ================================================================ parser

namespace {

class Parser {
 public:
  explicit Parser(std::vector<Token> toks) : t_(std::move(toks)) {}

  Result<Statement> run() {
    Statement st;
    if (is_kw("EXPLAIN")) {
      st.explain = true;
      next();
    }
    if (peek().kind != Token::Kind::Ident) {
      return syntax_error(peek().pos, "expected INSERT, SELECT, DELETE or EXPLAIN, found " + describe(peek()));
    }
    const std::string kw = upper(peek().text);
    Status s;
    if (kw == "INSERT") { next(); st.type = StatementType::Insert; s = parse_insert(st); }
    else if (kw == "SELECT") { next(); st.type = StatementType::Select; s = parse_select(st); }
    else if (kw == "DELETE") { next(); st.type = StatementType::Delete; s = parse_delete(st); }
    else if (kw == "UPDATE" || kw == "CREATE" || kw == "DROP" || kw == "ALTER" || kw == "TRUNCATE") {
      return Status::Error(Code::Unsupported, kw + " is not supported; the supported statements are INSERT, SELECT, DELETE and EXPLAIN");
    } else {
      return syntax_error(peek().pos, "expected INSERT, SELECT, DELETE or EXPLAIN, found '" + peek().text + "'");
    }
    if (!s.ok()) return s;
    if (is_sym(";")) next();
    if (peek().kind != Token::Kind::End) {
      return syntax_error(peek().pos, "unexpected " + describe(peek()) + " after the end of the statement (only one statement per request)");
    }
    return st;
  }

 private:
  const Token& peek() const { return t_[i_]; }
  Token next() {
    Token t = t_[i_];
    if (i_ + 1 < t_.size()) ++i_;
    return t;
  }
  bool is_kw(const char* kw) const { return peek().kind == Token::Kind::Ident && upper(peek().text) == kw; }
  bool is_sym(const char* s) const { return peek().kind == Token::Kind::Symbol && peek().text == s; }

  Status expect_kw(const char* kw) {
    if (!is_kw(kw)) return syntax_error(peek().pos, std::string("expected ") + kw + ", found " + describe(peek()));
    next();
    return Status::Ok();
  }
  Status expect_sym(const char* s) {
    if (!is_sym(s)) return syntax_error(peek().pos, std::string("expected '") + s + "', found " + describe(peek()));
    next();
    return Status::Ok();
  }
  Status expect_table() {
    if (peek().kind != Token::Kind::Ident || upper(peek().text) != "RECORDS") {
      return syntax_error(peek().pos, "unknown table " + describe(peek()) + "; the only table is 'records'");
    }
    next();
    return Status::Ok();
  }

  Status parse_insert(Statement& st) {
    Status s = expect_kw("INTO");
    if (!s.ok()) return s;
    if (!(s = expect_table()).ok()) return s;
    if (!(s = expect_sym("(")).ok()) return s;
    std::string cols[2];
    for (int k = 0; k < 2; ++k) {
      if (peek().kind != Token::Kind::Ident) return syntax_error(peek().pos, "expected a column name, found " + describe(peek()));
      cols[k] = upper(next().text);
      if (k == 0 && !(s = expect_sym(",")).ok()) return s;
    }
    if (!(s = expect_sym(")")).ok()) return s;
    bool key_first = cols[0] == "KEY" && cols[1] == "PAYLOAD";
    bool payload_first = cols[0] == "PAYLOAD" && cols[1] == "KEY";
    if (!key_first && !payload_first) {
      return syntax_error(t_[i_].pos, "INSERT needs exactly the columns (key, payload)");
    }
    if (!(s = expect_kw("VALUES")).ok()) return s;
    if (!(s = expect_sym("(")).ok()) return s;
    for (int k = 0; k < 2; ++k) {
      const bool want_key = (k == 0) == key_first;
      const Token v = peek();
      if (want_key) {
        if (v.kind != Token::Kind::Integer) return syntax_error(v.pos, "key must be an integer, found " + describe(v));
        st.key = v.int_value;
      } else {
        if (v.kind != Token::Kind::String) return syntax_error(v.pos, "payload must be a quoted string, found " + describe(v));
        st.payload = v.text;
      }
      next();
      if (k == 0 && !(s = expect_sym(",")).ok()) return s;
    }
    return expect_sym(")");
  }

  // Intersects `r` with (key op v).
  static void apply(KeyRange& r, const std::string& op, Key v) {
    constexpr Key kMin = std::numeric_limits<Key>::min();
    constexpr Key kMax = std::numeric_limits<Key>::max();
    if (op == "=") { r.lo = std::max(r.lo, v); r.hi = std::min(r.hi, v); }
    else if (op == ">=") r.lo = std::max(r.lo, v);
    else if (op == "<=") r.hi = std::min(r.hi, v);
    else if (op == ">") { if (v == kMax) r.empty = true; else r.lo = std::max(r.lo, v + 1); }
    else if (op == "<") { if (v == kMin) r.empty = true; else r.hi = std::min(r.hi, v - 1); }
    if (r.lo > r.hi) r.empty = true;
  }

  Status parse_condition(KeyRange& r) {
    if (peek().kind != Token::Kind::Ident) return syntax_error(peek().pos, "expected a condition on 'key', found " + describe(peek()));
    if (upper(peek().text) != "KEY") {
      return syntax_error(peek().pos, "only column 'key' can be used in WHERE, found '" + peek().text + "'");
    }
    next();
    if (is_kw("BETWEEN")) {
      next();
      if (peek().kind != Token::Kind::Integer) return syntax_error(peek().pos, "BETWEEN needs an integer, found " + describe(peek()));
      Key a = next().int_value;
      Status s = expect_kw("AND");
      if (!s.ok()) return s;
      if (peek().kind != Token::Kind::Integer) return syntax_error(peek().pos, "BETWEEN needs an integer, found " + describe(peek()));
      Key b = next().int_value;
      apply(r, ">=", a);
      apply(r, "<=", b);
      return Status::Ok();
    }
    if (peek().kind != Token::Kind::Symbol ||
        (peek().text != "=" && peek().text != "<" && peek().text != "<=" && peek().text != ">" && peek().text != ">=")) {
      return syntax_error(peek().pos, "expected one of = < <= > >= BETWEEN after 'key', found " + describe(peek()));
    }
    const std::string op = next().text;
    if (peek().kind != Token::Kind::Integer) return syntax_error(peek().pos, "expected an integer after '" + op + "', found " + describe(peek()));
    apply(r, op, next().int_value);
    return Status::Ok();
  }

  Status parse_where(Statement& st) {
    st.has_where = true;
    Status s = parse_condition(st.range);
    if (!s.ok()) return s;
    while (true) {
      if (is_kw("AND")) {
        next();
        if (!(s = parse_condition(st.range)).ok()) return s;
      } else if (is_kw("OR")) {
        return Status::Error(Code::Unsupported, "OR is not supported; combine conditions with AND (at position " + std::to_string(peek().pos) + ")");
      } else {
        break;
      }
    }
    return Status::Ok();
  }

  Status parse_select(Statement& st) {
    if (is_kw("DISTINCT") || peek().kind == Token::Kind::Ident) {
      return Status::Error(Code::Unsupported, "only SELECT * is supported (at position " + std::to_string(peek().pos) + ")");
    }
    Status s = expect_sym("*");
    if (!s.ok()) return s;
    if (!(s = expect_kw("FROM")).ok()) return s;
    if (!(s = expect_table()).ok()) return s;
    if (is_kw("WHERE")) {
      next();
      if (!(s = parse_where(st)).ok()) return s;
    }
    if (is_kw("ORDER") || is_kw("GROUP") || is_kw("JOIN")) {
      return Status::Error(Code::Unsupported, upper(peek().text) + " is not supported; results are always ordered by key");
    }
    if (is_kw("LIMIT")) {
      next();
      if (peek().kind != Token::Kind::Integer || peek().int_value < 0) {
        return syntax_error(peek().pos, "LIMIT needs a non-negative integer, found " + describe(peek()));
      }
      st.limit = static_cast<std::uint64_t>(next().int_value);
    }
    return Status::Ok();
  }

  Status parse_delete(Statement& st) {
    Status s = expect_kw("FROM");
    if (!s.ok()) return s;
    if (!(s = expect_table()).ok()) return s;
    if (!is_kw("WHERE")) return syntax_error(peek().pos, "DELETE needs a WHERE key = <integer> clause, found " + describe(peek()));
    next();
    if (!(s = parse_where(st)).ok()) return s;
    if (!st.range.is_point()) {
      return Status::Error(Code::Unsupported, "DELETE supports only WHERE key = <integer>");
    }
    return Status::Ok();
  }

  std::vector<Token> t_;
  std::size_t i_ = 0;
};

}  // namespace

Result<Statement> parse(const std::string& sql) {
  Result<std::vector<Token>> toks = tokenize(sql);
  if (!toks.ok()) return toks.status();
  Parser p(toks.take());
  return p.run();
}

// ================================================================ planner

namespace {

std::string pct(double fraction) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(fraction < 0.01 ? 3 : 1) << fraction * 100.0 << "%";
  return os.str();
}

std::optional<std::size_t> effective_cap(const Statement& s, const ExecOptions& o) {
  std::optional<std::size_t> cap;
  if (s.limit) cap = static_cast<std::size_t>(std::min<std::uint64_t>(*s.limit, std::numeric_limits<std::size_t>::max()));
  if (o.max_rows > 0) cap = cap ? std::min(*cap, o.max_rows) : o.max_rows;
  return cap;
}

}  // namespace

Result<Plan> QueryEngine::plan(const Statement& st, ForcePath force) const {
  if (!db_.is_open()) return Status::Error(Code::InvalidArgument, "database is not open");
  Plan p;
  p.stmt = st;
  const BPlusTree& idx = db_.index();
  const double n = static_cast<double>(idx.size());
  const std::size_t h = idx.height();

  if (st.type == StatementType::Insert) {
    if (force != ForcePath::Auto) return Status::Error(Code::Unsupported, "an access path can only be forced for SELECT");
    p.path = AccessPath::IndexInsert;
    p.explain = {
        "Access path: INDEX_INSERT (B+ Tree maintained on every write)",
        "Step 1: probe the B+ Tree (height " + std::to_string(h) + ") to reject a duplicate key",
        "Step 2: append the record to the heap file, or reuse a deleted slot that fits",
        "Step 3: insert key -> (record id, file offset) into the B+ Tree, splitting nodes if they overflow",
    };
    return p;
  }
  if (st.type == StatementType::Delete) {
    if (force != ForcePath::Auto) return Status::Error(Code::Unsupported, "an access path can only be forced for SELECT");
    p.path = AccessPath::IndexPoint;
    p.estimated_matches = 1;
    p.explain = {
        "Access path: INDEX_POINT (locate the record through the B+ Tree)",
        "Step 1: descend the B+ Tree (height " + std::to_string(h) + ") to find the record locator for the key",
        "Step 2: mark the heap slot deleted (tombstone) in place",
        "Step 3: remove the key from the B+ Tree, redistributing or merging nodes on underflow",
    };
    return p;
  }

  // ---- SELECT
  const KeyRange& r = st.range;
  std::string reason;
  if (r.empty) {
    p.estimated_matches = 0;
  } else if (!st.has_where || r.is_unbounded()) {
    p.estimated_matches = n;
  } else if (r.is_point()) {
    p.estimated_matches = n > 0 ? 1 : 0;
  } else if (n > 0) {
    const double kmin = static_cast<double>(*idx.min_key());
    const double kmax = static_cast<double>(*idx.max_key());
    const double lo = std::max(static_cast<double>(r.lo), kmin);
    const double hi = std::min(static_cast<double>(r.hi), kmax);
    const double span = kmax - kmin + 1.0;
    p.estimated_matches = hi >= lo ? n * ((hi - lo + 1.0) / span) : 0.0;
  }
  p.estimated_selectivity = n > 0 ? std::min(1.0, p.estimated_matches / n) : 0.0;

  if (force == ForcePath::Scan) {
    p.path = AccessPath::FullScan;
    p.forced = true;
    reason = "forced by request (ForcePath::Scan)";
  } else if (force == ForcePath::Index) {
    p.path = r.is_point() ? AccessPath::IndexPoint : AccessPath::IndexRange;
    p.forced = true;
    reason = "forced by request (ForcePath::Index)";
  } else if (!st.has_where || r.is_unbounded()) {
    p.path = AccessPath::FullScan;
    reason = "no predicate on the indexed key, so every record qualifies";
  } else if (r.is_point()) {
    p.path = AccessPath::IndexPoint;
    reason = "equality predicate on the indexed key";
  } else if (p.estimated_selectivity > kScanSelectivityThreshold) {
    p.path = AccessPath::FullScan;
    reason = "estimated selectivity " + pct(p.estimated_selectivity) + " exceeds the " +
             pct(kScanSelectivityThreshold) + " threshold; one sequential pass is cheaper than many random record fetches";
  } else {
    p.path = AccessPath::IndexRange;
    reason = "range predicate on the indexed key with estimated selectivity " + pct(p.estimated_selectivity) +
             " (<= " + pct(kScanSelectivityThreshold) + ")";
  }

  switch (p.path) {
    case AccessPath::IndexPoint:
      p.explain = {"Access path: INDEX_POINT (B+ Tree exact-match lookup)",
                   "Reason: " + reason,
                   "Step 1: descend the B+ Tree (height " + std::to_string(h) + ") to the leaf that may hold the key",
                   "Step 2: if present, read that one record slot from the heap file at the stored offset",
                   "Estimated cost: ~" + std::to_string(h) + " tree nodes + 1 heap read"};
      break;
    case AccessPath::IndexRange:
      p.explain = {"Access path: INDEX_RANGE (B+ Tree range scan over linked leaves)",
                   "Reason: " + reason,
                   "Step 1: descend the B+ Tree to the leaf containing the lower bound",
                   "Step 2: walk the leaf chain in key order until a key exceeds the upper bound",
                   "Step 3: fetch each matching record from the heap file (one random read per record)",
                   "Estimated matches: ~" + std::to_string(static_cast<std::uint64_t>(p.estimated_matches + 0.5)) + " of " +
                       std::to_string(idx.size()) + " records (" + pct(p.estimated_selectivity) + ")"};
      break;
    default:
      p.explain = {"Access path: FULL_SCAN (sequential heap-file scan)",
                   "Reason: " + reason,
                   "Step 1: read every slot of the heap file in order (" + std::to_string(db_.heap().live_count()) +
                       " live records, " + std::to_string(db_.heap().file_size()) + " bytes), verifying each record's CRC",
                   "Step 2: evaluate the key predicate on each live record",
                   "Step 3: sort the matches by key and apply LIMIT",
                   "Estimated matches: ~" + std::to_string(static_cast<std::uint64_t>(p.estimated_matches + 0.5)) + " of " +
                       std::to_string(idx.size()) + " records"};
      break;
  }
  if (st.limit) p.explain.push_back("LIMIT " + std::to_string(*st.limit) + " applies to rows in ascending key order");
  return p;
}

// ================================================================ executor

QueryResult QueryEngine::exec_select(const Plan& plan, const ExecOptions& opts) {
  QueryResult res;
  res.type = StatementType::Select;
  res.path = plan.path;
  res.path_forced = plan.forced;
  res.plan = plan.explain;
  res.executed = true;

  const Statement& s = plan.stmt;
  const KeyRange& r = s.range;
  const auto cap = effective_cap(s, opts);
  const std::uint64_t t0 = now_ns();

  if (r.empty || (cap && *cap == 0)) {
    // Contradictory predicate or LIMIT 0: no work needed.
  } else if (plan.path == AccessPath::FullScan) {
    auto by_key = [](const Row& a, const Row& b) { return a.key < b.key; };
    std::uint64_t examined = 0, matched = 0;
    Status st = db_.heap().scan_raw([&](Key key, RecordId rid, const char* payload, std::uint32_t len, std::uint64_t) {
      ++examined;
      if (key < r.lo || key > r.hi) return true;
      ++matched;
      if (!cap) {
        res.rows.push_back(Row{key, rid, std::string(payload, len)});
      } else if (res.rows.size() < *cap) {
        res.rows.push_back(Row{key, rid, std::string(payload, len)});
        std::push_heap(res.rows.begin(), res.rows.end(), by_key);  // max-heap on key keeps the smallest `cap`
      } else if (key < res.rows.front().key) {
        std::pop_heap(res.rows.begin(), res.rows.end(), by_key);
        res.rows.back() = Row{key, rid, std::string(payload, len)};
        std::push_heap(res.rows.begin(), res.rows.end(), by_key);
      }
      return true;
    });
    if (!st.ok()) res.status = st;
    std::sort(res.rows.begin(), res.rows.end(), by_key);
    res.records_examined = examined;
    res.rows_matched = matched;
    res.has_more = matched > res.rows.size();
  } else {
    const std::uint64_t reads_before = db_.heap().read_calls();
    BPlusTree::SearchStats ss;
    if (plan.path == AccessPath::IndexPoint) {
      auto loc = db_.index().find(r.lo, &ss);
      res.index_lookups = 1;
      if (loc) {
        Result<Record> rec = db_.heap().read_at(*loc);
        if (rec.ok()) {
          res.rows.push_back(Row{rec.value().key, rec.value().rid, rec.value().payload});
          res.records_examined = 1;
          res.rows_matched = 1;
        } else {
          res.status = rec.status();
        }
      }
    } else {
      std::vector<RecordLocator> locs;
      bool more = false;
      db_.index().for_each_in_range(
          r.lo, r.hi,
          [&](Key, const RecordLocator& loc) {
            if (cap && locs.size() >= *cap) { more = true; return false; }
            locs.push_back(loc);
            return true;
          },
          &ss);
      res.index_lookups = 1;
      res.rows.reserve(locs.size());
      for (const RecordLocator& loc : locs) {
        Result<Record> rec = db_.heap().read_at(loc);
        if (!rec.ok()) { res.status = rec.status(); break; }
        res.rows.push_back(Row{rec.value().key, rec.value().rid, rec.take().payload});
      }
      res.records_examined = res.rows.size();
      res.rows_matched = locs.size() + (more ? 1 : 0);
      res.has_more = more;
    }
    res.index_nodes_visited = ss.nodes_visited;
    res.heap_reads = db_.heap().read_calls() - reads_before;
  }
  res.exec_ns = now_ns() - t0;
  if (res.ok()) {
    res.message = std::to_string(res.rows.size()) + " row(s) returned" + (res.has_more ? " (more rows match; result is capped)" : "");
    res.plan.push_back("Actual: examined " + std::to_string(res.records_examined) + " record(s), " +
                       std::to_string(res.index_nodes_visited) + " index node visit(s), " +
                       std::to_string(res.heap_reads) + " heap read call(s), returned " + std::to_string(res.rows.size()) + " row(s)");
  }
  return res;
}

QueryResult QueryEngine::exec_insert(const Plan& plan) {
  QueryResult res;
  res.type = StatementType::Insert;
  res.path = plan.path;
  res.plan = plan.explain;
  res.executed = true;
  const std::uint64_t t0 = now_ns();
  RecordLocator loc;
  BPlusTree::SearchStats ss;
  (void)db_.index().find(plan.stmt.key, &ss);  // same probe Database::insert performs; counted for reporting
  res.status = db_.insert(plan.stmt.key, plan.stmt.payload, &loc);
  res.exec_ns = now_ns() - t0;
  res.index_lookups = 1;
  res.index_nodes_visited = ss.nodes_visited;
  if (res.ok()) {
    res.rows_affected = 1;
    res.message = "inserted key " + std::to_string(plan.stmt.key) + " as record " + std::to_string(loc.rid);
  }
  return res;
}

QueryResult QueryEngine::exec_delete(const Plan& plan) {
  QueryResult res;
  res.type = StatementType::Delete;
  res.path = plan.path;
  res.plan = plan.explain;
  res.executed = true;
  const Key key = plan.stmt.range.lo;
  const std::uint64_t t0 = now_ns();
  BPlusTree::SearchStats ss;
  auto loc = db_.index().find(key, &ss);
  res.index_lookups = 1;
  res.index_nodes_visited = ss.nodes_visited;
  if (!loc) {
    res.message = "0 rows deleted: no record with key " + std::to_string(key);
  } else {
    res.status = db_.erase(key);
    if (res.ok()) {
      res.rows_affected = 1;
      res.records_examined = 1;
      res.message = "deleted key " + std::to_string(key);
    }
  }
  res.exec_ns = now_ns() - t0;
  return res;
}

QueryResult QueryEngine::execute_plan(const Plan& plan, const ExecOptions& opts) {
  switch (plan.stmt.type) {
    case StatementType::Insert: return exec_insert(plan);
    case StatementType::Delete: return exec_delete(plan);
    case StatementType::Select: return exec_select(plan, opts);
  }
  QueryResult bad;
  bad.status = Status::Error(Code::Internal, "unknown statement type");
  return bad;
}

QueryResult QueryEngine::execute(const std::string& sql, const ExecOptions& opts) {
  const std::uint64_t p0 = now_ns();
  Result<Statement> parsed = parse(sql);
  const std::uint64_t parse_ns = now_ns() - p0;
  if (!parsed.ok()) {
    QueryResult bad;
    bad.status = parsed.status();
    bad.type_known = false;
    bad.parse_ns = parse_ns;
    return bad;
  }
  const Statement& st = parsed.value();
  const std::uint64_t q0 = now_ns();
  Result<Plan> pl = plan(st, opts.force);
  const std::uint64_t plan_ns = now_ns() - q0;
  if (!pl.ok()) {
    QueryResult bad;
    bad.type = st.type;
    bad.status = pl.status();
    bad.parse_ns = parse_ns;
    return bad;
  }
  if (st.explain) {
    QueryResult res;
    res.type = st.type;
    res.path = pl.value().path;
    res.path_forced = pl.value().forced;
    res.plan = pl.value().explain;
    res.parse_ns = parse_ns;
    res.exec_ns = plan_ns;
    res.executed = false;
    res.message = "EXPLAIN: statement was planned but not executed";
    return res;
  }
  QueryResult res = execute_plan(pl.value(), opts);
  res.parse_ns = parse_ns;
  res.exec_ns += plan_ns;
  return res;
}

CompareResult QueryEngine::compare(const std::string& sql, const ExecOptions& opts) {
  CompareResult out;
  const std::uint64_t p0 = now_ns();
  Result<Statement> parsed = parse(sql);
  const std::uint64_t parse_ns = now_ns() - p0;
  if (!parsed.ok()) { out.status = parsed.status(); return out; }
  const Statement& st = parsed.value();
  if (st.type != StatementType::Select || st.explain) {
    out.status = Status::Error(Code::Unsupported, "scan-versus-index comparison is only available for SELECT statements");
    return out;
  }
  Result<Plan> scan_plan = plan(st, ForcePath::Scan);
  Result<Plan> index_plan = plan(st, ForcePath::Index);
  if (!scan_plan.ok()) { out.status = scan_plan.status(); return out; }
  if (!index_plan.ok()) { out.status = index_plan.status(); return out; }

  out.scan = execute_plan(scan_plan.value(), opts);
  out.index = execute_plan(index_plan.value(), opts);
  out.scan.parse_ns = out.index.parse_ns = parse_ns;
  if (!out.scan.ok()) { out.status = out.scan.status; return out; }
  if (!out.index.ok()) { out.status = out.index.status; return out; }

  out.results_match = true;
  if (out.scan.rows.size() != out.index.rows.size()) {
    out.results_match = false;
    out.mismatch = "row counts differ: scan returned " + std::to_string(out.scan.rows.size()) + ", index returned " +
                   std::to_string(out.index.rows.size());
  } else {
    for (std::size_t i = 0; i < out.scan.rows.size(); ++i) {
      const Row& a = out.scan.rows[i];
      const Row& b = out.index.rows[i];
      if (a.key != b.key || a.rid != b.rid || a.payload != b.payload) {
        out.results_match = false;
        out.mismatch = "row " + std::to_string(i) + " differs (scan key " + std::to_string(a.key) + ", index key " + std::to_string(b.key) + ")";
        break;
      }
    }
  }
  if (out.results_match && out.scan.has_more != out.index.has_more) {
    out.results_match = false;
    out.mismatch = "has_more flags differ between the two paths";
  }
  out.speedup = static_cast<double>(out.scan.exec_ns) / static_cast<double>(std::max<std::uint64_t>(out.index.exec_ns, 1));
  return out;
}

// ================================================================ JSON

namespace {

void write_error(std::ostringstream& os, const Status& s) {
  if (s.ok()) { os << "null"; return; }
  os << "{\"code\":" << json_quote(code_name(s.code())) << ",\"message\":" << json_quote(s.message()) << "}";
}

void write_result(std::ostringstream& os, const QueryResult& r) {
  os << "{\"ok\":" << (r.ok() ? "true" : "false") << ",\"error\":";
  write_error(os, r.status);
  os << ",\"statement\":";
  if (r.type_known) os << json_quote(statement_type_name(r.type)); else os << "null";
  os << ""
     << ",\"access_path\":" << json_quote(access_path_name(r.path))
     << ",\"path_forced\":" << (r.path_forced ? "true" : "false")
     << ",\"executed\":" << (r.executed ? "true" : "false") << ",\"rows\":[";
  for (std::size_t i = 0; i < r.rows.size(); ++i) {
    os << (i ? "," : "") << "{\"key\":" << r.rows[i].key << ",\"rid\":" << r.rows[i].rid
       << ",\"payload\":" << json_quote(r.rows[i].payload) << "}";
  }
  os << "],\"row_count\":" << r.rows.size() << ",\"rows_matched\":" << r.rows_matched
     << ",\"has_more\":" << (r.has_more ? "true" : "false") << ",\"rows_affected\":" << r.rows_affected
     << ",\"records_examined\":" << r.records_examined << ",\"index_lookups\":" << r.index_lookups
     << ",\"index_nodes_visited\":" << r.index_nodes_visited << ",\"heap_reads\":" << r.heap_reads
     << ",\"parse_ns\":" << r.parse_ns << ",\"exec_ns\":" << r.exec_ns
     << ",\"total_ns\":" << (r.parse_ns + r.exec_ns)
     << ",\"duration_ms\":" << static_cast<double>(r.exec_ns) / 1e6 << ",\"plan\":[";
  for (std::size_t i = 0; i < r.plan.size(); ++i) os << (i ? "," : "") << json_quote(r.plan[i]);
  os << "],\"message\":" << json_quote(r.message) << "}";
}

}  // namespace

std::string to_json(const QueryResult& r) {
  std::ostringstream os;
  write_result(os, r);
  return os.str();
}

std::string to_json(const CompareResult& r) {
  std::ostringstream os;
  os << "{\"ok\":" << (r.ok() ? "true" : "false") << ",\"error\":";
  write_error(os, r.status);
  os << ",\"results_match\":" << (r.results_match ? "true" : "false") << ",\"mismatch\":" << json_quote(r.mismatch)
     << ",\"speedup\":" << r.speedup << ",\"scan\":";
  write_result(os, r.scan);
  os << ",\"index\":";
  write_result(os, r.index);
  os << "}";
  return os.str();
}

}  // namespace indexcraft
