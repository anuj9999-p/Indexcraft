#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>

#include "indexcraft/bplus_tree.hpp"
#include "test_util.hpp"

using namespace indexcraft;

namespace {

RecordLocator loc_for(Key k) { return RecordLocator{static_cast<RecordId>(k < 0 ? -k : k) + 1, static_cast<std::uint64_t>(k < 0 ? -k : k) * 8 + 64}; }

void expect_valid(const BPlusTree& t) {
  auto v = t.validate();
  EXPECT_TRUE(v.empty()) << (v.empty() ? "" : v.front());
}

std::vector<Key> all_keys(const BPlusTree& t) {
  std::vector<Key> out;
  t.for_each(0, [&](Key k, const RecordLocator&) { out.push_back(k); return true; });
  return out;
}

}  // namespace

TEST(BPlusTreeBasics, EmptyTree) {
  BPlusTree t(4);
  EXPECT_EQ(t.size(), 0u);
  EXPECT_EQ(t.height(), 1u);
  EXPECT_EQ(t.node_count(), 1u);
  EXPECT_FALSE(t.find(1).has_value());
  EXPECT_TRUE(t.range(0, 100).empty());
  EXPECT_FALSE(t.min_key().has_value());
  EXPECT_FALSE(t.max_key().has_value());
  EXPECT_FALSE(t.erase(1));
  expect_valid(t);
}

TEST(BPlusTreeBasics, InsertAndFind) {
  BPlusTree t(4);
  EXPECT_TRUE(t.insert(42, RecordLocator{7, 1000}));
  auto r = t.find(42);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->rid, 7u);
  EXPECT_EQ(r->offset, 1000u);
  EXPECT_FALSE(t.find(43).has_value());
  EXPECT_EQ(*t.min_key(), 42);
  EXPECT_EQ(*t.max_key(), 42);
}

TEST(BPlusTreeBasics, DuplicateKeyIsRejectedAndLeavesTreeUntouched) {
  BPlusTree t(3);
  for (Key k = 1; k <= 20; ++k) ASSERT_TRUE(t.insert(k, loc_for(k)));
  std::string before = t.to_json();
  EXPECT_FALSE(t.insert(10, RecordLocator{999, 999}));
  EXPECT_EQ(t.size(), 20u);
  EXPECT_EQ(t.find(10)->rid, loc_for(10).rid);  // original locator preserved
  EXPECT_EQ(t.to_json(), before);
  expect_valid(t);
}

TEST(BPlusTreeBasics, MissingKeyEraseReturnsFalse) {
  BPlusTree t(4);
  for (Key k = 0; k < 10; ++k) t.insert(k * 2, loc_for(k));
  EXPECT_FALSE(t.erase(3));
  EXPECT_EQ(t.size(), 10u);
}

TEST(BPlusTreeBasics, RejectsTinyCapacity) { EXPECT_THROW(BPlusTree(2), std::invalid_argument); }

TEST(BPlusTreeBasics, ExtremeKeysWork) {
  BPlusTree t(3);
  const Key lo = std::numeric_limits<Key>::min(), hi = std::numeric_limits<Key>::max();
  for (Key k : {lo, hi, Key{0}, Key{-1}, Key{1}}) ASSERT_TRUE(t.insert(k, loc_for(1)));
  EXPECT_TRUE(t.find(lo).has_value());
  EXPECT_TRUE(t.find(hi).has_value());
  EXPECT_EQ(t.range(lo, hi).size(), 5u);
  EXPECT_EQ(*t.min_key(), lo);
  EXPECT_EQ(*t.max_key(), hi);
  expect_valid(t);
}

// ---------------------------------------------------------------- structure events

TEST(BPlusTreeStructure, LeafSplitAndRootSplit) {
  BPlusTree t(4);
  for (Key k = 1; k <= 4; ++k) t.insert(k, loc_for(k));
  EXPECT_EQ(t.height(), 1u);
  EXPECT_EQ(t.counters().leaf_splits, 0u);
  t.insert(5, loc_for(5));  // 5th key overflows the only leaf: it splits and the root splits too
  EXPECT_EQ(t.counters().leaf_splits, 1u);
  EXPECT_EQ(t.counters().root_splits, 1u);
  EXPECT_EQ(t.height(), 2u);
  EXPECT_EQ(t.leaf_count(), 2u);
  EXPECT_EQ(t.internal_count(), 1u);
  EXPECT_EQ(t.node_count(), 3u);
  expect_valid(t);
  // The separator is a copy of the first key of the right leaf: left [1,2,3], right [4,5].
  std::string json = t.to_json();
  EXPECT_NE(json.find("\"keys\":[4]"), std::string::npos) << json;
  EXPECT_NE(json.find("\"keys\":[1,2,3]"), std::string::npos) << json;
  EXPECT_NE(json.find("\"keys\":[4,5]"), std::string::npos) << json;
}

TEST(BPlusTreeStructure, InternalNodeSplitGrowsHeightToThree) {
  BPlusTree t(3);
  Key k = 0;
  while (t.counters().internal_splits == 0) {
    ++k;
    ASSERT_TRUE(t.insert(k, loc_for(k)));
    ASSERT_LT(k, 1000);
    expect_valid(t);
  }
  EXPECT_GE(t.counters().internal_splits, 1u);
  EXPECT_EQ(t.height(), 3u);
  EXPECT_EQ(t.counters().root_splits, 2u);  // root split once for height 2, once more for height 3
  EXPECT_EQ(t.size(), static_cast<std::size_t>(k));
}

TEST(BPlusTreeStructure, LeafRedistribution) {
  BPlusTree t(4);
  for (Key k = 1; k <= 12; ++k) t.insert(k, loc_for(k));  // leaves [1,2,3] [4,5,6] [7,8,9] [10,11,12]
  ASSERT_EQ(t.counters().leaf_redistributions, 0u);
  ASSERT_TRUE(t.erase(2));  // [1,3] still at minimum (2)
  EXPECT_EQ(t.counters().leaf_redistributions, 0u);
  ASSERT_TRUE(t.erase(1));  // [3] underflows; right sibling has 3 keys, so it lends one
  EXPECT_EQ(t.counters().leaf_redistributions, 1u);
  EXPECT_EQ(t.counters().leaf_merges, 0u);
  expect_valid(t);
  EXPECT_FALSE(t.find(1).has_value());
  EXPECT_TRUE(t.find(3).has_value());
  EXPECT_TRUE(t.find(4).has_value());
}

TEST(BPlusTreeStructure, LeafMergeAndRootShrink) {
  BPlusTree t(4);
  for (Key k = 1; k <= 5; ++k) t.insert(k, loc_for(k));  // [1,2,3] [4,5], height 2
  ASSERT_EQ(t.height(), 2u);
  ASSERT_TRUE(t.erase(5));  // [4] underflows; left sibling [1,2,3] has a spare key -> redistribute: [1,2] [3,4]
  EXPECT_EQ(t.counters().leaf_redistributions, 1u);
  ASSERT_TRUE(t.erase(1));  // [2] underflows; right sibling [3,4] is at minimum -> merge into [2,3,4]
  EXPECT_EQ(t.counters().leaf_merges, 1u);
  ASSERT_TRUE(t.erase(2));
  EXPECT_EQ(t.counters().root_shrinks, 1u);  // the internal root lost its last separator at the merge
  EXPECT_EQ(t.height(), 1u);
  EXPECT_EQ(t.node_count(), 1u);
  expect_valid(t);
  EXPECT_EQ(t.size(), 2u);
}

TEST(BPlusTreeStructure, InternalRedistributionAndMergeOccurAndStayValid) {
  for (std::size_t m : {3u, 4u, 5u, 6u}) {
    BPlusTree t(m);
    std::vector<Key> keys(600);
    std::iota(keys.begin(), keys.end(), 1);
    std::mt19937_64 rng(m);
    std::shuffle(keys.begin(), keys.end(), rng);
    for (Key k : keys) ASSERT_TRUE(t.insert(k, loc_for(k)));
    std::shuffle(keys.begin(), keys.end(), rng);
    std::size_t step = 0;
    for (Key k : keys) {
      ASSERT_TRUE(t.erase(k));
      if (++step % 10 == 0) { auto v = t.validate(); ASSERT_TRUE(v.empty()) << "m=" << m << " " << v.front(); }
    }
    EXPECT_EQ(t.size(), 0u);
    EXPECT_EQ(t.height(), 1u) << "m=" << m;
    EXPECT_EQ(t.node_count(), 1u);
    const auto& c = t.counters();
    EXPECT_GT(c.leaf_merges, 0u) << "m=" << m;
    EXPECT_GT(c.leaf_redistributions, 0u) << "m=" << m;
    EXPECT_GT(c.internal_merges, 0u) << "m=" << m;
    EXPECT_GT(c.internal_redistributions, 0u) << "m=" << m;
    EXPECT_GT(c.root_shrinks, 0u) << "m=" << m;
  }
}

// ---------------------------------------------------------------- insertion orders (parameterised by node capacity)

class InsertionOrder : public ::testing::TestWithParam<std::size_t> {};

TEST_P(InsertionOrder, Ascending) {
  BPlusTree t(GetParam());
  for (Key k = 1; k <= 3000; ++k) {
    ASSERT_TRUE(t.insert(k, loc_for(k)));
    if (k % 100 == 0) expect_valid(t);
  }
  expect_valid(t);
  for (Key k = 1; k <= 3000; ++k) ASSERT_EQ(t.find(k)->rid, loc_for(k).rid);
  auto keys = all_keys(t);
  ASSERT_EQ(keys.size(), 3000u);
  EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
}

TEST_P(InsertionOrder, Descending) {
  BPlusTree t(GetParam());
  for (Key k = 3000; k >= 1; --k) {
    ASSERT_TRUE(t.insert(k, loc_for(k)));
    if (k % 100 == 0) expect_valid(t);
  }
  expect_valid(t);
  auto keys = all_keys(t);
  ASSERT_EQ(keys.size(), 3000u);
  EXPECT_EQ(keys.front(), 1);
  EXPECT_EQ(keys.back(), 3000);
  EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
}

TEST_P(InsertionOrder, RandomThenRandomDelete) {
  BPlusTree t(GetParam());
  std::vector<Key> keys(3000);
  std::iota(keys.begin(), keys.end(), -1500);
  std::mt19937_64 rng(GetParam() * 31 + 7);
  std::shuffle(keys.begin(), keys.end(), rng);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    ASSERT_TRUE(t.insert(keys[i], loc_for(keys[i])));
    if (i % 250 == 0) expect_valid(t);
  }
  expect_valid(t);
  std::vector<Key> sorted = keys;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(all_keys(t), sorted);

  std::shuffle(keys.begin(), keys.end(), rng);
  std::set<Key> remaining(sorted.begin(), sorted.end());
  for (std::size_t i = 0; i < 2700; ++i) {
    ASSERT_TRUE(t.erase(keys[i]));
    remaining.erase(keys[i]);
    if (i % 100 == 0) expect_valid(t);
  }
  expect_valid(t);
  EXPECT_EQ(all_keys(t), std::vector<Key>(remaining.begin(), remaining.end()));
  for (std::size_t i = 0; i < 2700; ++i) EXPECT_FALSE(t.find(keys[i]).has_value());
}

INSTANTIATE_TEST_SUITE_P(Capacities, InsertionOrder, ::testing::Values(3u, 4u, 5u, 8u, 16u, 64u));

// ---------------------------------------------------------------- range queries

TEST(BPlusTreeRange, BoundariesAreInclusive) {
  BPlusTree t(3);
  for (Key k : {10, 20, 30, 40, 50}) t.insert(k, loc_for(k));
  auto keys_of = [&](Key lo, Key hi, std::size_t limit = 0) {
    std::vector<Key> out;
    for (auto& e : t.range(lo, hi, limit)) out.push_back(e.first);
    return out;
  };
  EXPECT_EQ(keys_of(20, 40), (std::vector<Key>{20, 30, 40}));
  EXPECT_EQ(keys_of(21, 39), (std::vector<Key>{30}));
  EXPECT_EQ(keys_of(10, 10), (std::vector<Key>{10}));
  EXPECT_EQ(keys_of(50, 50), (std::vector<Key>{50}));
  EXPECT_EQ(keys_of(11, 19), (std::vector<Key>{}));
  EXPECT_EQ(keys_of(51, 100), (std::vector<Key>{}));
  EXPECT_EQ(keys_of(-100, 9), (std::vector<Key>{}));
  EXPECT_EQ(keys_of(40, 20), (std::vector<Key>{}));  // lo > hi
  EXPECT_EQ(keys_of(std::numeric_limits<Key>::min(), std::numeric_limits<Key>::max()).size(), 5u);
  EXPECT_EQ(keys_of(0, 100, 2), (std::vector<Key>{10, 20}));  // limit
}

TEST(BPlusTreeRange, MatchesReferenceAcrossManyLeaves) {
  BPlusTree t(4);
  std::map<Key, int> ref;
  std::mt19937_64 rng(5);
  for (int i = 0; i < 2000; ++i) {
    Key k = static_cast<Key>(rng() % 5000);
    if (t.insert(k, loc_for(k))) ref[k] = 1;
  }
  for (int i = 0; i < 500; ++i) {
    Key a = static_cast<Key>(rng() % 6000) - 500, b = static_cast<Key>(rng() % 6000) - 500;
    std::vector<Key> got, want;
    for (auto& e : t.range(a, b)) got.push_back(e.first);
    for (auto it = ref.lower_bound(a); it != ref.end() && it->first <= b; ++it) want.push_back(it->first);
    if (a > b) want.clear();
    ASSERT_EQ(got, want) << "range [" << a << "," << b << "]";
  }
}

TEST(BPlusTreeRange, ForEachOffsetSkipsLeaves) {
  BPlusTree t(4);
  for (Key k = 0; k < 100; ++k) t.insert(k, loc_for(k));
  std::vector<Key> got;
  t.for_each(37, [&](Key k, const RecordLocator&) { got.push_back(k); return got.size() < 5; });
  EXPECT_EQ(got, (std::vector<Key>{37, 38, 39, 40, 41}));
  got.clear();
  t.for_each(100, [&](Key k, const RecordLocator&) { got.push_back(k); return true; });
  EXPECT_TRUE(got.empty());
}

// ---------------------------------------------------------------- introspection

TEST(BPlusTreeIntrospection, SearchPathEndsAtLeafAndMatchesHeight) {
  BPlusTree t(4);
  for (Key k = 1; k <= 200; ++k) t.insert(k, loc_for(k));
  auto p = t.search_path(77);
  EXPECT_TRUE(p.found);
  EXPECT_EQ(p.node_ids.size(), t.height());
  auto miss = t.search_path(1000);
  EXPECT_FALSE(miss.found);
  EXPECT_EQ(miss.node_ids.size(), t.height());
  BPlusTree::SearchStats st;
  t.find(77, &st);
  EXPECT_EQ(st.nodes_visited, t.height());
}

TEST(BPlusTreeIntrospection, JsonDescribesTheRealStructure) {
  BPlusTree t(4);
  for (Key k = 1; k <= 50; ++k) t.insert(k, loc_for(k));
  std::string j = t.to_json();
  auto count = [&](const std::string& needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = j.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
    return n;
  };
  EXPECT_EQ(count("\"leaf\":true"), t.leaf_count());
  EXPECT_EQ(count("\"leaf\":false"), t.internal_count());
  EXPECT_NE(j.find("\"truncated\":false"), std::string::npos);
  std::string small = t.to_json(3);
  EXPECT_NE(small.find("\"truncated\":true"), std::string::npos);
  EXPECT_NE(small.find("\"nodes_returned\":3"), std::string::npos);
}

TEST(BPlusTreeIntrospection, ClearResetsEverything) {
  BPlusTree t(3);
  for (Key k = 1; k <= 100; ++k) t.insert(k, loc_for(k));
  t.clear();
  EXPECT_EQ(t.size(), 0u);
  EXPECT_EQ(t.node_count(), 1u);
  EXPECT_EQ(t.counters().leaf_splits, 0u);
  expect_valid(t);
  EXPECT_TRUE(t.insert(5, loc_for(5)));
}

// ---------------------------------------------------------------- reference-model randomised test

class TreeReferenceModel : public ::testing::TestWithParam<std::size_t> {};

TEST_P(TreeReferenceModel, RandomOperationsMatchStdMap) {
  BPlusTree t(GetParam());
  std::map<Key, RecordLocator> ref;
  std::mt19937_64 rng(GetParam() * 1009);
  for (int i = 0; i < 60000; ++i) {
    Key k = static_cast<Key>(rng() % 800) - 400;
    switch (rng() % 4) {
      case 0:
      case 1: {
        RecordLocator l{static_cast<RecordId>(i), static_cast<std::uint64_t>(i) * 3};
        bool inserted = t.insert(k, l);
        ASSERT_EQ(inserted, ref.find(k) == ref.end());
        if (inserted) ref[k] = l;
        break;
      }
      case 2: ASSERT_EQ(t.erase(k), ref.erase(k) == 1); break;
      default: {
        auto got = t.find(k);
        auto it = ref.find(k);
        ASSERT_EQ(got.has_value(), it != ref.end());
        if (got) { ASSERT_EQ(got->rid, it->second.rid); }
      }
    }
    if (i % 2000 == 0) {
      expect_valid(t);
      ASSERT_EQ(t.size(), ref.size());
      std::vector<Key> want;
      for (auto& e : ref) want.push_back(e.first);
      ASSERT_EQ(all_keys(t), want);
    }
  }
  expect_valid(t);
}

INSTANTIATE_TEST_SUITE_P(Capacities, TreeReferenceModel, ::testing::Values(3u, 4u, 7u, 32u));

TEST(BPlusTreeLarge, MillionKeysInsertValidateDeleteHalf) {
  BPlusTree t(64);
  const std::size_t n = 1000000;
  std::vector<Key> keys(n);
  std::iota(keys.begin(), keys.end(), 1);
  std::mt19937_64 rng(99);
  for (std::size_t i = n - 1; i > 0; --i) std::swap(keys[i], keys[rng() % (i + 1)]);
  for (Key k : keys) ASSERT_TRUE(t.insert(k, loc_for(k)));
  EXPECT_EQ(t.size(), n);
  expect_valid(t);
  EXPECT_LE(t.height(), 5u);
  for (int i = 0; i < 20000; ++i) {
    Key k = static_cast<Key>(1 + rng() % n);
    ASSERT_EQ(t.find(k)->rid, loc_for(k).rid);
  }
  EXPECT_EQ(t.range(1000, 1999).size(), 1000u);
  for (std::size_t i = 0; i < n / 2; ++i) ASSERT_TRUE(t.erase(keys[i]));
  EXPECT_EQ(t.size(), n / 2);
  expect_valid(t);
}
