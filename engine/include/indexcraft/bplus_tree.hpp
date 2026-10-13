#pragma once
// BPlusTree: an in-memory B+ Tree mapping unique int64 keys to RecordLocators.
//
// Parameter: max_keys (M >= 3) = maximum number of keys in ANY node.
//   * Leaf node:     holds 1..M keys with their locators; leaves are chained left-to-right.
//   * Internal node: holds k keys and k+1 children, child i contains keys with
//                    keys[i-1] <= key < keys[i].
//   * Minimum occupancy (non-root): leaf ceil(M/2) keys, internal floor(M/2) keys.
//   * Split of an overflowing leaf (M+1 keys): left keeps ceil((M+1)/2), right gets the
//     rest; the right node's first key is COPIED up as separator.
//   * Split of an overflowing internal node (M+1 keys): the middle key is MOVED up.
//   * Underflow after delete: borrow one entry from a sibling that has spare entries
//     (redistribution, separator updated in the parent), otherwise merge with a sibling
//     and pull the separator down. A root with a single child is removed (root shrink).
// Keys are unique; insert of an existing key is rejected.
//
// The tree lives in memory only. Persistence is the heap file's job: the index is
// rebuilt from the live heap records when the database is opened.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "indexcraft/types.hpp"

namespace indexcraft {

class BPlusTree {
 public:
  struct SearchStats {
    std::uint64_t nodes_visited = 0;
  };

  // Structural event counters since construction / clear(). Used by tests and Diagnostics.
  struct Counters {
    std::uint64_t leaf_splits = 0;
    std::uint64_t internal_splits = 0;
    std::uint64_t root_splits = 0;
    std::uint64_t leaf_redistributions = 0;
    std::uint64_t internal_redistributions = 0;
    std::uint64_t leaf_merges = 0;
    std::uint64_t internal_merges = 0;
    std::uint64_t root_shrinks = 0;
  };

  struct SearchPath {
    std::vector<std::uint64_t> node_ids;  // root first, leaf last
    bool found = false;
  };

  using EntryVisitor = std::function<bool(Key, const RecordLocator&)>;  // false = stop

  explicit BPlusTree(std::size_t max_keys = 64);  // throws std::invalid_argument if max_keys < 3
  ~BPlusTree();
  BPlusTree(const BPlusTree&) = delete;
  BPlusTree& operator=(const BPlusTree&) = delete;

  bool insert(Key key, const RecordLocator& loc);  // false if the key already exists
  std::optional<RecordLocator> find(Key key, SearchStats* stats = nullptr) const;
  bool erase(Key key);                              // false if the key does not exist
  void clear();

  // Visits entries with lo <= key <= hi in ascending order. Returns entries visited.
  std::size_t for_each_in_range(Key lo, Key hi, const EntryVisitor& visitor,
                                SearchStats* stats = nullptr) const;
  // Collects up to `limit` entries (0 = unlimited) with lo <= key <= hi.
  std::vector<std::pair<Key, RecordLocator>> range(Key lo, Key hi, std::size_t limit = 0,
                                                   SearchStats* stats = nullptr) const;
  // Visits all entries in key order, skipping the first `offset` entries cheaply.
  void for_each(std::size_t offset, const EntryVisitor& visitor) const;

  std::optional<Key> min_key() const;
  std::optional<Key> max_key() const;

  std::size_t size() const { return size_; }
  std::size_t height() const;  // 1 for a single leaf
  std::size_t node_count() const { return leaf_count_ + internal_count_; }
  std::size_t leaf_count() const { return leaf_count_; }
  std::size_t internal_count() const { return internal_count_; }
  std::size_t max_keys() const { return max_keys_; }
  const Counters& counters() const { return counters_; }

  SearchPath search_path(Key key) const;

  // Structural representation as JSON (see docs). At most `max_nodes` nodes are emitted
  // in breadth-first order; "truncated" is true if some nodes were left out.
  std::string to_json(std::size_t max_nodes = 5000) const;

  // Checks every structural invariant. Returns a list of violations (empty = valid).
  std::vector<std::string> validate() const;

 private:
  struct Node {
    bool leaf = true;
    std::uint64_t id = 0;
    std::vector<Key> keys;
    std::vector<RecordLocator> values;              // leaf only, parallel to keys
    std::vector<std::unique_ptr<Node>> children;    // internal only, keys.size()+1 entries
    Node* next = nullptr;                           // leaf only: right sibling
  };
  struct Split {
    std::unique_ptr<Node> right;
    Key sep = 0;
  };

  std::unique_ptr<Node> make_node(bool leaf);
  std::size_t min_keys(const Node* n) const { return n->leaf ? (max_keys_ + 1) / 2 : max_keys_ / 2; }
  static std::size_t child_index(const Node* n, Key key);

  bool insert_rec(Node* node, Key key, const RecordLocator& loc, Split& split);
  bool erase_rec(Node* node, Key key);
  void rebalance_child(Node* parent, std::size_t idx);
  void borrow_from_left(Node* parent, std::size_t idx);
  void borrow_from_right(Node* parent, std::size_t idx);
  void merge_children(Node* parent, std::size_t left_idx);
  const Node* leaf_for(Key key, SearchStats* stats) const;
  const Node* leftmost_leaf() const;

  std::size_t max_keys_;
  std::unique_ptr<Node> root_;
  std::size_t size_ = 0;
  std::size_t leaf_count_ = 0;
  std::size_t internal_count_ = 0;
  std::uint64_t next_id_ = 1;
  Counters counters_;
};

}  // namespace indexcraft
