#include <algorithm>
#include <limits>
#include <map>
#include <random>

#include "indexcraft/json.hpp"
#include "indexcraft/query.hpp"
#include "test_util.hpp"

using namespace indexcraft;
using namespace testutil;

namespace {

struct Fixture {
  TempDir dir;
  Database db;
  QueryEngine engine{db};
  Fixture() {
    DatabaseOptions o;
    o.max_keys = 4;
    EXPECT_TRUE(db.open(dir.file("q.dat"), o).ok());
  }
  QueryResult run(const std::string& sql, ForcePath fp = ForcePath::Auto, std::size_t max_rows = 1000) {
    ExecOptions o;
    o.force = fp;
    o.max_rows = max_rows;
    return engine.execute(sql, o);
  }
  void load(Key from, Key to, Key step = 1) {
    for (Key k = from; k <= to; k += step) ASSERT_TRUE(db.insert(k, "payload-" + std::to_string(k)).ok());
  }
};

std::vector<Key> keys_of(const QueryResult& r) {
  std::vector<Key> k;
  for (auto& row : r.rows) k.push_back(row.key);
  return k;
}

}  // namespace

// ---------------------------------------------------------------- tokenizer

TEST(Tokenizer, BasicTokens) {
  auto t = tokenize("SELECT * FROM records WHERE key >= -5 AND key <= 10;");
  ASSERT_TRUE(t.ok());
  const auto& v = t.value();
  EXPECT_EQ(v.front().kind, Token::Kind::Ident);
  EXPECT_EQ(v[1].text, "*");
  EXPECT_EQ(v[6].text, ">=");
  EXPECT_EQ(v[7].kind, Token::Kind::Integer);
  EXPECT_EQ(v[7].int_value, -5);
  EXPECT_EQ(v.back().kind, Token::Kind::End);
}

TEST(Tokenizer, StringsSupportDoubledQuotesAndBothQuoteStyles) {
  auto t = tokenize("'it''s' \"say \"\"hi\"\"\" ''");
  ASSERT_TRUE(t.ok());
  EXPECT_EQ(t.value()[0].text, "it's");
  EXPECT_EQ(t.value()[1].text, "say \"hi\"");
  EXPECT_EQ(t.value()[2].text, "");
}

TEST(Tokenizer, Errors) {
  EXPECT_EQ(tokenize("'unterminated").status().code(), Code::SyntaxError);
  EXPECT_EQ(tokenize("SELECT #").status().code(), Code::SyntaxError);
  EXPECT_EQ(tokenize("key = 1.5").status().code(), Code::SyntaxError);
  EXPECT_EQ(tokenize("key = 12abc").status().code(), Code::SyntaxError);
  EXPECT_EQ(tokenize("key = 99999999999999999999").status().code(), Code::SyntaxError);
  EXPECT_EQ(tokenize("key <> 1").status().code(), Code::Unsupported);
  EXPECT_EQ(tokenize("key != 1").status().code(), Code::Unsupported);
  EXPECT_TRUE(tokenize("key = -9223372036854775808").ok());
  EXPECT_TRUE(tokenize("key = 9223372036854775807").ok());
}

// ---------------------------------------------------------------- parser

TEST(Parser, InsertForms) {
  auto a = parse("INSERT INTO records (key, payload) VALUES (5, 'hello world')");
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a.value().type, StatementType::Insert);
  EXPECT_EQ(a.value().key, 5);
  EXPECT_EQ(a.value().payload, "hello world");
  auto b = parse("insert into RECORDS (payload, KEY) values ('x', -3);");
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(b.value().key, -3);
  EXPECT_EQ(b.value().payload, "x");
  auto c = parse("INSERT INTO records (key, payload) VALUES (1, 'it''s')");
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.value().payload, "it's");
}

TEST(Parser, SelectRangeNormalisation) {
  auto r = [](const std::string& where) {
    auto s = parse("SELECT * FROM records WHERE " + where);
    EXPECT_TRUE(s.ok()) << where << ": " << s.status().to_string();
    return s.ok() ? s.value().range : KeyRange{};
  };
  auto eq = r("key = 5");
  EXPECT_TRUE(eq.is_point());
  EXPECT_EQ(eq.lo, 5);
  auto both = r("key >= 10 AND key <= 20");
  EXPECT_EQ(both.lo, 10);
  EXPECT_EQ(both.hi, 20);
  auto strict = r("key > 5 AND key < 8");
  EXPECT_EQ(strict.lo, 6);
  EXPECT_EQ(strict.hi, 7);
  auto between = r("key BETWEEN 3 AND 9");
  EXPECT_EQ(between.lo, 3);
  EXPECT_EQ(between.hi, 9);
  EXPECT_TRUE(r("key > 9223372036854775807").empty);
  EXPECT_TRUE(r("key < -9223372036854775808").empty);
  EXPECT_TRUE(r("key >= 10 AND key <= 5").empty);
  EXPECT_TRUE(r("key = 1 AND key = 2").empty);
  auto tight = r("key >= 5 AND key = 5");
  EXPECT_TRUE(tight.is_point());
}

TEST(Parser, SelectLimitCaseAndSemicolon) {
  auto s = parse("  select  *  from  records  where  key  >=  1  limit  25 ; ");
  ASSERT_TRUE(s.ok());
  ASSERT_TRUE(s.value().limit.has_value());
  EXPECT_EQ(*s.value().limit, 25u);
  EXPECT_TRUE(parse("SELECT * FROM records").ok());
  EXPECT_FALSE(parse("SELECT * FROM records").value().has_where);
}

TEST(Parser, ExplainFlag) {
  auto s = parse("EXPLAIN SELECT * FROM records WHERE key = 1");
  ASSERT_TRUE(s.ok());
  EXPECT_TRUE(s.value().explain);
}

TEST(Parser, InvalidQueriesProduceMeaningfulErrors) {
  struct Case { const char* sql; Code code; const char* fragment; };
  const Case cases[] = {
      {"", Code::SyntaxError, "expected INSERT"},
      {"   ", Code::SyntaxError, "end of input"},
      {"SELEC * FROM records", Code::SyntaxError, "SELEC"},
      {"SELECT * FROM", Code::SyntaxError, "unknown table"},
      {"SELECT * FROM users", Code::SyntaxError, "only table is 'records'"},
      {"SELECT * records", Code::SyntaxError, "expected FROM"},
      {"SELECT key FROM records", Code::Unsupported, "only SELECT *"},
      {"SELECT * FROM records WHERE", Code::SyntaxError, "expected a condition"},
      {"SELECT * FROM records WHERE payload = 'x'", Code::SyntaxError, "only column 'key'"},
      {"SELECT * FROM records WHERE key", Code::SyntaxError, "expected one of"},
      {"SELECT * FROM records WHERE key = 'abc'", Code::SyntaxError, "expected an integer"},
      {"SELECT * FROM records WHERE key = 1 OR key = 2", Code::Unsupported, "OR is not supported"},
      {"SELECT * FROM records WHERE key = 1 AND", Code::SyntaxError, "expected a condition"},
      {"SELECT * FROM records WHERE key BETWEEN 1 5", Code::SyntaxError, "expected AND"},
      {"SELECT * FROM records LIMIT", Code::SyntaxError, "LIMIT needs"},
      {"SELECT * FROM records LIMIT -1", Code::SyntaxError, "LIMIT needs"},
      {"SELECT * FROM records LIMIT 'x'", Code::SyntaxError, "LIMIT needs"},
      {"SELECT * FROM records ORDER BY key", Code::Unsupported, "ORDER"},
      {"SELECT * FROM records; SELECT * FROM records", Code::SyntaxError, "only one statement"},
      {"SELECT * FROM records garbage", Code::SyntaxError, "unexpected"},
      {"INSERT INTO records (key) VALUES (1)", Code::SyntaxError, "expected ','"},
      {"INSERT INTO records (key, payload) VALUES ('a', 'b')", Code::SyntaxError, "key must be an integer"},
      {"INSERT INTO records (key, payload) VALUES (1, 2)", Code::SyntaxError, "payload must be a quoted string"},
      {"INSERT INTO records (key, payload) VALUES (1)", Code::SyntaxError, "expected ','"},
      {"INSERT INTO records (key, payload) VALUES (1, 'x'", Code::SyntaxError, "expected ')'"},
      {"INSERT INTO records (a, b) VALUES (1, 'x')", Code::SyntaxError, "exactly the columns"},
      {"INSERT records (key, payload) VALUES (1, 'x')", Code::SyntaxError, "expected INTO"},
      {"INSERT INTO records (key, payload) VALUES (1, 'x", Code::SyntaxError, "unterminated"},
      {"DELETE FROM records", Code::SyntaxError, "needs a WHERE"},
      {"DELETE FROM records WHERE key >= 1", Code::Unsupported, "only WHERE key ="},
      {"DELETE FROM records WHERE key = 1 AND key = 2", Code::Unsupported, "only WHERE key ="},
      {"UPDATE records SET payload = 'x' WHERE key = 1", Code::Unsupported, "UPDATE is not supported"},
      {"DROP TABLE records", Code::Unsupported, "DROP"},
      {"hello world", Code::SyntaxError, "expected INSERT"},
      {"12345", Code::SyntaxError, "expected INSERT"},
  };
  for (const Case& c : cases) {
    auto r = parse(c.sql);
    ASSERT_FALSE(r.ok()) << "should have failed: " << c.sql;
    EXPECT_EQ(r.status().code(), c.code) << c.sql << " -> " << r.status().to_string();
    EXPECT_NE(r.status().message().find(c.fragment), std::string::npos) << c.sql << " -> " << r.status().to_string();
  }
}

// ---------------------------------------------------------------- execution and access paths

TEST(Executor, InsertSelectDeleteRoundTrip) {
  Fixture f;
  auto ins = f.run("INSERT INTO records (key, payload) VALUES (10, 'ten')");
  ASSERT_TRUE(ins.ok()) << ins.status.to_string();
  EXPECT_EQ(ins.rows_affected, 1u);
  EXPECT_EQ(ins.path, AccessPath::IndexInsert);
  auto sel = f.run("SELECT * FROM records WHERE key = 10");
  ASSERT_TRUE(sel.ok());
  ASSERT_EQ(sel.rows.size(), 1u);
  EXPECT_EQ(sel.rows[0].payload, "ten");
  auto dup = f.run("INSERT INTO records (key, payload) VALUES (10, 'again')");
  EXPECT_FALSE(dup.ok());
  EXPECT_EQ(dup.status.code(), Code::DuplicateKey);
  auto del = f.run("DELETE FROM records WHERE key = 10");
  ASSERT_TRUE(del.ok());
  EXPECT_EQ(del.rows_affected, 1u);
  EXPECT_TRUE(f.run("SELECT * FROM records WHERE key = 10").rows.empty());
  auto del2 = f.run("DELETE FROM records WHERE key = 10");
  EXPECT_TRUE(del2.ok());
  EXPECT_EQ(del2.rows_affected, 0u);  // deleting a missing key affects zero rows
}

TEST(Executor, AutomaticAccessPathSelection) {
  Fixture f;
  f.load(1, 1000);
  EXPECT_EQ(f.run("SELECT * FROM records WHERE key = 500").path, AccessPath::IndexPoint);
  EXPECT_EQ(f.run("SELECT * FROM records WHERE key >= 100 AND key <= 150").path, AccessPath::IndexRange);
  EXPECT_EQ(f.run("SELECT * FROM records WHERE key >= 1 AND key <= 600").path, AccessPath::FullScan);  // 60% > 25%
  EXPECT_EQ(f.run("SELECT * FROM records WHERE key >= 1 AND key <= 250").path, AccessPath::IndexRange);  // exactly 25%
  EXPECT_EQ(f.run("SELECT * FROM records").path, AccessPath::FullScan);
  EXPECT_EQ(f.run("SELECT * FROM records WHERE key > 900").path, AccessPath::IndexRange);
  EXPECT_EQ(f.run("SELECT * FROM records WHERE key >= 5000").path, AccessPath::IndexRange);  // nothing qualifies
}

TEST(Executor, ForcedPathsAndReportedMetrics) {
  Fixture f;
  f.load(1, 500);
  auto scan = f.run("SELECT * FROM records WHERE key = 250", ForcePath::Scan);
  auto idx = f.run("SELECT * FROM records WHERE key = 250", ForcePath::Index);
  ASSERT_TRUE(scan.ok() && idx.ok());
  EXPECT_EQ(scan.path, AccessPath::FullScan);
  EXPECT_TRUE(scan.path_forced);
  EXPECT_EQ(scan.records_examined, 500u);  // the scan visited every live record
  EXPECT_EQ(scan.index_lookups, 0u);
  EXPECT_EQ(idx.path, AccessPath::IndexPoint);
  EXPECT_EQ(idx.records_examined, 1u);
  EXPECT_EQ(idx.index_lookups, 1u);
  EXPECT_EQ(idx.index_nodes_visited, f.db.index().height());
  EXPECT_GE(idx.heap_reads, 1u);
  EXPECT_GT(scan.exec_ns, 0u);
  EXPECT_GT(idx.exec_ns, 0u);
  EXPECT_EQ(keys_of(scan), keys_of(idx));
  EXPECT_FALSE(idx.plan.empty());
}

TEST(Executor, ForcingAPathOnWritesIsRejected) {
  Fixture f;
  auto r = f.run("INSERT INTO records (key, payload) VALUES (1, 'x')", ForcePath::Scan);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status.code(), Code::Unsupported);
  EXPECT_EQ(f.db.stats().record_count, 0u);
}

TEST(Executor, ExplainPlansWithoutExecuting) {
  Fixture f;
  auto r = f.run("EXPLAIN INSERT INTO records (key, payload) VALUES (1, 'x')");
  ASSERT_TRUE(r.ok());
  EXPECT_FALSE(r.executed);
  EXPECT_FALSE(r.plan.empty());
  EXPECT_EQ(f.db.stats().record_count, 0u);  // nothing was inserted
  f.load(1, 100);
  auto d = f.run("EXPLAIN DELETE FROM records WHERE key = 5");
  EXPECT_FALSE(d.executed);
  EXPECT_TRUE(f.db.get(5).ok());
}

TEST(Executor, LimitTakesTheSmallestKeysOnBothPaths) {
  Fixture f;
  std::vector<Key> keys;
  std::mt19937_64 rng(4);
  for (int i = 0; i < 300; ++i) keys.push_back(static_cast<Key>(rng() % 100000));
  for (Key k : keys) f.db.insert(k, "p");
  for (ForcePath fp : {ForcePath::Scan, ForcePath::Index}) {
    auto r = f.run("SELECT * FROM records LIMIT 7", fp);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.rows.size(), 7u);
    const std::vector<Key> got = keys_of(r);
    EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
    std::vector<Key> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    EXPECT_EQ(keys_of(r), std::vector<Key>(sorted.begin(), sorted.begin() + 7));
    EXPECT_TRUE(r.has_more);
  }
  EXPECT_TRUE(f.run("SELECT * FROM records LIMIT 0").rows.empty());
}

TEST(Executor, MaxRowsCapsResultsAndSetsHasMore) {
  Fixture f;
  f.load(1, 200);
  for (ForcePath fp : {ForcePath::Scan, ForcePath::Index}) {
    auto r = f.run("SELECT * FROM records WHERE key >= 1 AND key <= 150", fp, 20);
    EXPECT_EQ(r.rows.size(), 20u);
    EXPECT_TRUE(r.has_more);
    EXPECT_EQ(r.rows.front().key, 1);
    EXPECT_EQ(r.rows.back().key, 20);
    auto all = f.run("SELECT * FROM records WHERE key >= 1 AND key <= 150", fp, 0);
    EXPECT_EQ(all.rows.size(), 150u);
    EXPECT_FALSE(all.has_more);
  }
}

TEST(Executor, EmptyAndContradictoryRanges) {
  Fixture f;
  f.load(1, 50);
  for (ForcePath fp : {ForcePath::Auto, ForcePath::Scan, ForcePath::Index}) {
    EXPECT_TRUE(f.run("SELECT * FROM records WHERE key >= 40 AND key <= 10", fp).rows.empty());
    EXPECT_TRUE(f.run("SELECT * FROM records WHERE key = 1 AND key = 2", fp).rows.empty());
    EXPECT_TRUE(f.run("SELECT * FROM records WHERE key > 9223372036854775807", fp).rows.empty());
    EXPECT_TRUE(f.run("SELECT * FROM records WHERE key = 999", fp).rows.empty());
    EXPECT_EQ(f.run("SELECT * FROM records WHERE key >= -5 AND key <= 3", fp).rows.size(), 3u);
  }
}

TEST(Executor, QueriesOnAnEmptyDatabase) {
  Fixture f;
  for (ForcePath fp : {ForcePath::Auto, ForcePath::Scan, ForcePath::Index}) {
    EXPECT_TRUE(f.run("SELECT * FROM records", fp).ok());
    EXPECT_TRUE(f.run("SELECT * FROM records WHERE key = 1", fp).rows.empty());
    EXPECT_TRUE(f.run("SELECT * FROM records WHERE key >= 1 AND key <= 9", fp).rows.empty());
  }
}

TEST(Executor, PayloadsWithQuotesAndUnicodeSurviveARoundTrip) {
  Fixture f;
  ASSERT_TRUE(f.run("INSERT INTO records (key, payload) VALUES (1, 'it''s \"quoted\" caf\xC3\xA9 \xE2\x9C\x93')").ok());
  auto r = f.run("SELECT * FROM records WHERE key = 1");
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].payload, "it's \"quoted\" caf\xC3\xA9 \xE2\x9C\x93");
}

TEST(Executor, ResultsSurviveRestart) {
  Fixture f;
  f.run("INSERT INTO records (key, payload) VALUES (42, 'persisted')");
  f.db.close();
  DatabaseOptions o;
  o.max_keys = 4;
  ASSERT_TRUE(f.db.open(f.dir.file("q.dat"), o).ok());
  auto r = f.run("SELECT * FROM records WHERE key = 42");
  ASSERT_EQ(r.rows.size(), 1u);
  EXPECT_EQ(r.rows[0].payload, "persisted");
}

// ---------------------------------------------------------------- scan versus index equivalence

TEST(Compare, RejectsNonSelect) {
  Fixture f;
  auto c = f.engine.compare("DELETE FROM records WHERE key = 1");
  EXPECT_FALSE(c.ok());
  EXPECT_EQ(c.status.code(), Code::Unsupported);
  EXPECT_FALSE(f.engine.compare("garbage").ok());
}

TEST(Compare, ReportsBothPathsAndAMatch) {
  Fixture f;
  f.load(1, 2000);
  auto c = f.engine.compare("SELECT * FROM records WHERE key >= 100 AND key <= 140");
  ASSERT_TRUE(c.ok());
  EXPECT_TRUE(c.results_match) << c.mismatch;
  EXPECT_EQ(c.scan.path, AccessPath::FullScan);
  EXPECT_EQ(c.index.path, AccessPath::IndexRange);
  EXPECT_EQ(c.scan.records_examined, 2000u);
  EXPECT_EQ(c.index.records_examined, 41u);
  EXPECT_EQ(c.scan.rows.size(), 41u);
  EXPECT_GT(c.speedup, 0.0);
}

TEST(Compare, ScanAndIndexAgreeWithAReferenceModelOnRandomQueries) {
  Fixture f;
  std::map<Key, std::string> ref;
  std::mt19937_64 rng(2024);
  for (int i = 0; i < 4000; ++i) {
    Key k = static_cast<Key>(rng() % 20001) - 10000;
    std::string p = "p" + std::to_string(i);
    if (f.db.insert(k, p).ok()) ref[k] = p;
  }
  for (int i = 0; i < 700; ++i) {  // churn: deletions create reusable slots and tombstones
    Key k = static_cast<Key>(rng() % 20001) - 10000;
    if (f.db.erase(k).ok()) ref.erase(k);
  }
  for (int i = 0; i < 300; ++i) {
    Key k = static_cast<Key>(rng() % 20001) - 10000;
    std::string p = "late" + std::to_string(i);
    if (f.db.insert(k, p).ok()) ref[k] = p;
  }
  ASSERT_TRUE(f.db.check_consistency().consistent);

  auto expected = [&](Key lo, Key hi, std::size_t cap) {
    std::vector<Key> out;
    for (auto it = ref.lower_bound(lo); it != ref.end() && it->first <= hi && lo <= hi; ++it) {
      if (cap && out.size() >= cap) break;
      out.push_back(it->first);
    }
    return out;
  };

  for (int i = 0; i < 400; ++i) {
    Key a = static_cast<Key>(rng() % 24001) - 12000, b = static_cast<Key>(rng() % 24001) - 12000;
    std::size_t kind = rng() % 4;
    std::string sql;
    Key lo = a, hi = b;
    if (kind == 0) { sql = "SELECT * FROM records WHERE key = " + std::to_string(a); hi = a; }
    else if (kind == 1) sql = "SELECT * FROM records WHERE key >= " + std::to_string(a) + " AND key <= " + std::to_string(b);
    else if (kind == 2) { sql = "SELECT * FROM records WHERE key BETWEEN " + std::to_string(a) + " AND " + std::to_string(b); }
    else { sql = "SELECT * FROM records WHERE key > " + std::to_string(a) + " AND key < " + std::to_string(b); lo = a + 1; hi = b - 1; }
    std::size_t limit = rng() % 3 == 0 ? 1 + rng() % 30 : 0;
    if (limit) sql += " LIMIT " + std::to_string(limit);
    std::size_t max_rows = rng() % 4 == 0 ? 1 + rng() % 50 : 0;
    ExecOptions o;
    o.max_rows = max_rows;
    CompareResult c = f.engine.compare(sql, o);
    ASSERT_TRUE(c.ok()) << sql << " -> " << c.status.to_string();
    ASSERT_TRUE(c.results_match) << sql << ": " << c.mismatch;
    std::size_t cap = limit;
    if (max_rows) cap = cap ? std::min(cap, max_rows) : max_rows;
    ASSERT_EQ(keys_of(c.scan), expected(lo, hi, cap)) << sql;
    for (std::size_t r = 0; r < c.scan.rows.size(); ++r) ASSERT_EQ(c.scan.rows[r].payload, ref[c.scan.rows[r].key]);
  }
}

TEST(Compare, EquivalenceHoldsAfterRestart) {
  Fixture f;
  f.load(1, 3000, 3);
  f.db.close();
  DatabaseOptions o;
  o.max_keys = 5;
  ASSERT_TRUE(f.db.open(f.dir.file("q.dat"), o).ok());
  auto c = f.engine.compare("SELECT * FROM records WHERE key >= 500 AND key <= 1500");
  ASSERT_TRUE(c.ok());
  EXPECT_TRUE(c.results_match) << c.mismatch;
  EXPECT_GT(c.scan.rows.size(), 0u);
}

// ---------------------------------------------------------------- JSON output

TEST(JsonOutput, QuotingEscapesAndSanitises) {
  EXPECT_EQ(json_quote("a\"b\\c\n\t"), "\"a\\\"b\\\\c\\n\\t\"");
  EXPECT_EQ(json_quote(std::string("x\x01y", 3)), "\"x\\u0001y\"");
  EXPECT_EQ(json_quote("caf\xC3\xA9"), "\"caf\xC3\xA9\"");   // valid UTF-8 kept
  EXPECT_EQ(json_quote("bad\xFF" "byte"), "\"bad\\ufffdbyte\"");  // invalid byte replaced
  EXPECT_EQ(json_quote("trunc\xE2\x9C"), "\"trunc\\ufffd\\ufffd\"");
}

TEST(JsonOutput, QueryResultContainsTheDocumentedFields) {
  Fixture f;
  f.load(1, 10);
  std::string j = to_json(f.run("SELECT * FROM records WHERE key = 3"));
  for (const char* field : {"\"ok\":true", "\"access_path\":\"INDEX_POINT\"", "\"records_examined\":1", "\"index_lookups\":1",
                            "\"exec_ns\":", "\"plan\":[", "\"rows\":[{\"key\":3"}) {
    EXPECT_NE(j.find(field), std::string::npos) << field << " missing in " << j;
  }
  std::string err = to_json(f.run("SELECT nonsense"));
  EXPECT_NE(err.find("\"ok\":false"), std::string::npos);
  EXPECT_NE(err.find("\"code\":\"UNSUPPORTED\""), std::string::npos);
}
