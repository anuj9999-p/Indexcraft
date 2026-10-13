#include "indexcraft/bplus_tree.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace indexcraft {

BPlusTree::BPlusTree(std::size_t max_keys) : max_keys_(max_keys) {
  if (max_keys < 3) throw std::invalid_argument("B+ Tree max_keys must be at least 3");
  root_ = make_node(true);
}

BPlusTree::~BPlusTree() = default;

std::unique_ptr<BPlusTree::Node> BPlusTree::make_node(bool leaf) {
  auto n = std::make_unique<Node>();
  n->leaf = leaf;
  n->id = next_id_++;
  if (leaf) ++leaf_count_; else ++internal_count_;
  return n;
}

void BPlusTree::clear() {
  root_.reset();
  size_ = 0;
  leaf_count_ = 0;
  internal_count_ = 0;
  next_id_ = 1;
  counters_ = Counters{};
  root_ = make_node(true);
}

std::size_t BPlusTree::child_index(const Node* n, Key key) {
  return static_cast<std::size_t>(std::upper_bound(n->keys.begin(), n->keys.end(), key) - n->keys.begin());
}

// ---------------------------------------------------------------- insert

bool BPlusTree::insert_rec(Node* node, Key key, const RecordLocator& loc, Split& split) {
  if (node->leaf) {
    auto it = std::lower_bound(node->keys.begin(), node->keys.end(), key);
    if (it != node->keys.end() && *it == key) return false;  // duplicate: nothing modified
    std::size_t pos = static_cast<std::size_t>(it - node->keys.begin());
    node->keys.insert(it, key);
    node->values.insert(node->values.begin() + static_cast<std::ptrdiff_t>(pos), loc);
    if (node->keys.size() > max_keys_) {
      std::size_t left_n = (node->keys.size() + 1) / 2;
      auto right = make_node(true);
      right->keys.assign(node->keys.begin() + static_cast<std::ptrdiff_t>(left_n), node->keys.end());
      right->values.assign(node->values.begin() + static_cast<std::ptrdiff_t>(left_n), node->values.end());
      node->keys.resize(left_n);
      node->values.resize(left_n);
      right->next = node->next;
      node->next = right.get();
      split.sep = right->keys.front();
      split.right = std::move(right);
      ++counters_.leaf_splits;
    }
    return true;
  }

  std::size_t idx = child_index(node, key);
  Split child_split;
  if (!insert_rec(node->children[idx].get(), key, loc, child_split)) return false;
  if (child_split.right) {
    node->keys.insert(node->keys.begin() + static_cast<std::ptrdiff_t>(idx), child_split.sep);
    node->children.insert(node->children.begin() + static_cast<std::ptrdiff_t>(idx) + 1,
                          std::move(child_split.right));
    if (node->keys.size() > max_keys_) {
      std::size_t mid = node->keys.size() / 2;
      auto right = make_node(false);
      split.sep = node->keys[mid];
      right->keys.assign(node->keys.begin() + static_cast<std::ptrdiff_t>(mid) + 1, node->keys.end());
      right->children.assign(
          std::make_move_iterator(node->children.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
          std::make_move_iterator(node->children.end()));
      node->keys.resize(mid);
      node->children.resize(mid + 1);
      split.right = std::move(right);
      ++counters_.internal_splits;
    }
  }
  return true;
}

bool BPlusTree::insert(Key key, const RecordLocator& loc) {
  Split split;
  if (!insert_rec(root_.get(), key, loc, split)) return false;
  if (split.right) {
    auto new_root = make_node(false);
    new_root->keys.push_back(split.sep);
    new_root->children.push_back(std::move(root_));
    new_root->children.push_back(std::move(split.right));
    root_ = std::move(new_root);
    ++counters_.root_splits;
  }
  ++size_;
  return true;
}

// ---------------------------------------------------------------- search

const BPlusTree::Node* BPlusTree::leaf_for(Key key, SearchStats* stats) const {
  const Node* n = root_.get();
  if (stats) ++stats->nodes_visited;
  while (!n->leaf) {
    n = n->children[child_index(n, key)].get();
    if (stats) ++stats->nodes_visited;
  }
  return n;
}

const BPlusTree::Node* BPlusTree::leftmost_leaf() const {
  const Node* n = root_.get();
  while (!n->leaf) n = n->children.front().get();
  return n;
}

std::optional<RecordLocator> BPlusTree::find(Key key, SearchStats* stats) const {
  const Node* leaf = leaf_for(key, stats);
  auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
  if (it == leaf->keys.end() || *it != key) return std::nullopt;
  return leaf->values[static_cast<std::size_t>(it - leaf->keys.begin())];
}

std::size_t BPlusTree::for_each_in_range(Key lo, Key hi, const EntryVisitor& visitor,
                                         SearchStats* stats) const {
  if (lo > hi) return 0;
  const Node* leaf = leaf_for(lo, stats);
  std::size_t visited = 0;
  std::size_t i = static_cast<std::size_t>(std::lower_bound(leaf->keys.begin(), leaf->keys.end(), lo) -
                                           leaf->keys.begin());
  while (leaf) {
    for (; i < leaf->keys.size(); ++i) {
      if (leaf->keys[i] > hi) return visited;
      ++visited;
      if (!visitor(leaf->keys[i], leaf->values[i])) return visited;
    }
    leaf = leaf->next;
    if (leaf && stats) ++stats->nodes_visited;
    i = 0;
  }
  return visited;
}

std::vector<std::pair<Key, RecordLocator>> BPlusTree::range(Key lo, Key hi, std::size_t limit,
                                                            SearchStats* stats) const {
  std::vector<std::pair<Key, RecordLocator>> out;
  for_each_in_range(
      lo, hi,
      [&](Key k, const RecordLocator& v) {
        out.emplace_back(k, v);
        return limit == 0 || out.size() < limit;
      },
      stats);
  return out;
}

void BPlusTree::for_each(std::size_t offset, const EntryVisitor& visitor) const {
  const Node* leaf = leftmost_leaf();
  while (leaf && offset >= leaf->keys.size()) {  // skip whole leaves without touching entries
    offset -= leaf->keys.size();
    leaf = leaf->next;
  }
  std::size_t i = offset;
  while (leaf) {
    for (; i < leaf->keys.size(); ++i) {
      if (!visitor(leaf->keys[i], leaf->values[i])) return;
    }
    leaf = leaf->next;
    i = 0;
  }
}

std::optional<Key> BPlusTree::min_key() const {
  if (size_ == 0) return std::nullopt;
  return leftmost_leaf()->keys.front();
}

std::optional<Key> BPlusTree::max_key() const {
  if (size_ == 0) return std::nullopt;
  const Node* n = root_.get();
  while (!n->leaf) n = n->children.back().get();
  return n->keys.back();
}

std::size_t BPlusTree::height() const {
  std::size_t h = 1;
  for (const Node* n = root_.get(); !n->leaf; n = n->children.front().get()) ++h;
  return h;
}

BPlusTree::SearchPath BPlusTree::search_path(Key key) const {
  SearchPath p;
  const Node* n = root_.get();
  p.node_ids.push_back(n->id);
  while (!n->leaf) {
    n = n->children[child_index(n, key)].get();
    p.node_ids.push_back(n->id);
  }
  p.found = std::binary_search(n->keys.begin(), n->keys.end(), key);
  return p;
}

// ---------------------------------------------------------------- erase

void BPlusTree::borrow_from_left(Node* parent, std::size_t idx) {
  Node* child = parent->children[idx].get();
  Node* left = parent->children[idx - 1].get();
  if (child->leaf) {
    child->keys.insert(child->keys.begin(), left->keys.back());
    child->values.insert(child->values.begin(), left->values.back());
    left->keys.pop_back();
    left->values.pop_back();
    parent->keys[idx - 1] = child->keys.front();
    ++counters_.leaf_redistributions;
  } else {
    child->keys.insert(child->keys.begin(), parent->keys[idx - 1]);
    child->children.insert(child->children.begin(), std::move(left->children.back()));
    left->children.pop_back();
    parent->keys[idx - 1] = left->keys.back();
    left->keys.pop_back();
    ++counters_.internal_redistributions;
  }
}

void BPlusTree::borrow_from_right(Node* parent, std::size_t idx) {
  Node* child = parent->children[idx].get();
  Node* right = parent->children[idx + 1].get();
  if (child->leaf) {
    child->keys.push_back(right->keys.front());
    child->values.push_back(right->values.front());
    right->keys.erase(right->keys.begin());
    right->values.erase(right->values.begin());
    parent->keys[idx] = right->keys.front();
    ++counters_.leaf_redistributions;
  } else {
    child->keys.push_back(parent->keys[idx]);
    child->children.push_back(std::move(right->children.front()));
    right->children.erase(right->children.begin());
    parent->keys[idx] = right->keys.front();
    right->keys.erase(right->keys.begin());
    ++counters_.internal_redistributions;
  }
}

// Merges children[left_idx + 1] into children[left_idx] and removes the separator.
void BPlusTree::merge_children(Node* parent, std::size_t left_idx) {
  Node* left = parent->children[left_idx].get();
  Node* right = parent->children[left_idx + 1].get();
  if (left->leaf) {
    left->keys.insert(left->keys.end(), right->keys.begin(), right->keys.end());
    left->values.insert(left->values.end(), right->values.begin(), right->values.end());
    left->next = right->next;
    --leaf_count_;
    ++counters_.leaf_merges;
  } else {
    left->keys.push_back(parent->keys[left_idx]);
    left->keys.insert(left->keys.end(), right->keys.begin(), right->keys.end());
    for (auto& c : right->children) left->children.push_back(std::move(c));
    --internal_count_;
    ++counters_.internal_merges;
  }
  parent->keys.erase(parent->keys.begin() + static_cast<std::ptrdiff_t>(left_idx));
  parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(left_idx) + 1);
}

void BPlusTree::rebalance_child(Node* parent, std::size_t idx) {
  Node* left = idx > 0 ? parent->children[idx - 1].get() : nullptr;
  Node* right = idx + 1 < parent->children.size() ? parent->children[idx + 1].get() : nullptr;
  if (left && left->keys.size() > min_keys(left)) {
    borrow_from_left(parent, idx);
  } else if (right && right->keys.size() > min_keys(right)) {
    borrow_from_right(parent, idx);
  } else if (left) {
    merge_children(parent, idx - 1);
  } else {
    merge_children(parent, idx);
  }
}

bool BPlusTree::erase_rec(Node* node, Key key) {
  if (node->leaf) {
    auto it = std::lower_bound(node->keys.begin(), node->keys.end(), key);
    if (it == node->keys.end() || *it != key) return false;
    std::ptrdiff_t pos = it - node->keys.begin();
    node->keys.erase(it);
    node->values.erase(node->values.begin() + pos);
    return true;
  }
  std::size_t idx = child_index(node, key);
  Node* child = node->children[idx].get();
  if (!erase_rec(child, key)) return false;
  if (child->keys.size() < min_keys(child)) rebalance_child(node, idx);
  return true;
}

bool BPlusTree::erase(Key key) {
  if (!erase_rec(root_.get(), key)) return false;
  --size_;
  while (!root_->leaf && root_->keys.empty()) {  // root shrink
    std::unique_ptr<Node> only_child = std::move(root_->children.front());
    root_ = std::move(only_child);
    --internal_count_;
    ++counters_.root_shrinks;
  }
  return true;
}

// ---------------------------------------------------------------- introspection

std::string BPlusTree::to_json(std::size_t max_nodes) const {
  struct Item {
    const Node* node;
    std::size_t level;
  };
  std::ostringstream os;
  std::deque<Item> queue;
  queue.push_back({root_.get(), 0});
  std::size_t emitted = 0;
  bool truncated = false;
  std::ostringstream nodes;
  bool first = true;
  while (!queue.empty()) {
    Item it = queue.front();
    queue.pop_front();
    if (emitted >= max_nodes) {
      truncated = true;
      break;
    }
    ++emitted;
    const Node* n = it.node;
    if (!first) nodes << ",";
    first = false;
    nodes << "{\"id\":" << n->id << ",\"leaf\":" << (n->leaf ? "true" : "false")
          << ",\"level\":" << it.level << ",\"keys\":[";
    for (std::size_t i = 0; i < n->keys.size(); ++i) nodes << (i ? "," : "") << n->keys[i];
    nodes << "],\"children\":[";
    if (!n->leaf) {
      for (std::size_t i = 0; i < n->children.size(); ++i) {
        nodes << (i ? "," : "") << n->children[i]->id;
        queue.push_back({n->children[i].get(), it.level + 1});
      }
    }
    nodes << "],\"next\":";
    if (n->next) nodes << n->next->id; else nodes << "null";
    nodes << ",\"occupancy\":" << static_cast<double>(n->keys.size()) / static_cast<double>(max_keys_)
          << "}";
  }
  os << "{\"max_keys\":" << max_keys_ << ",\"height\":" << height() << ",\"key_count\":" << size_
     << ",\"node_count\":" << node_count() << ",\"leaf_count\":" << leaf_count_
     << ",\"internal_count\":" << internal_count_ << ",\"root_id\":" << root_->id
     << ",\"nodes_returned\":" << emitted << ",\"truncated\":" << (truncated ? "true" : "false")
     << ",\"nodes\":[" << nodes.str() << "]}";
  return os.str();
}

namespace {
struct Checker {
  std::size_t max_keys;
  std::size_t min_leaf;
  std::size_t min_internal;
  std::vector<std::string> errors;
  long leaf_depth = -1;
  std::size_t keys_seen = 0;
  std::size_t leaves_seen = 0;
  std::size_t internals_seen = 0;
};
}  // namespace

std::vector<std::string> BPlusTree::validate() const {
  Checker c;
  c.max_keys = max_keys_;
  c.min_leaf = (max_keys_ + 1) / 2;
  c.min_internal = max_keys_ / 2;
  std::vector<const Node*> leaves_in_order;

  struct Frame {
    const Node* node;
    long depth;
    std::optional<Key> lo;  // inclusive lower bound
    std::optional<Key> hi;  // exclusive upper bound
  };
  std::function<void(const Frame&)> check = [&](const Frame& f) {
    const Node* n = f.node;
    const bool is_root = n == root_.get();
    const std::string name = std::string(n->leaf ? "leaf " : "internal ") + std::to_string(n->id);
    for (std::size_t i = 1; i < n->keys.size(); ++i) {
      if (!(n->keys[i - 1] < n->keys[i])) c.errors.push_back(name + ": keys not strictly ascending");
    }
    if (n->keys.size() > c.max_keys) c.errors.push_back(name + ": overflow (" + std::to_string(n->keys.size()) + " keys)");
    for (Key k : n->keys) {
      if ((f.lo && k < *f.lo) || (f.hi && k >= *f.hi)) {
        c.errors.push_back(name + ": key " + std::to_string(k) + " outside parent separator bounds");
      }
    }
    if (n->leaf) {
      ++c.leaves_seen;
      c.keys_seen += n->keys.size();
      leaves_in_order.push_back(n);
      if (n->values.size() != n->keys.size()) c.errors.push_back(name + ": values/keys size mismatch");
      if (!n->children.empty()) c.errors.push_back(name + ": leaf has children");
      if (!is_root && n->keys.size() < c.min_leaf) {
        c.errors.push_back(name + ": underflow (" + std::to_string(n->keys.size()) + " < " + std::to_string(c.min_leaf) + ")");
      }
      if (c.leaf_depth < 0) c.leaf_depth = f.depth;
      else if (c.leaf_depth != f.depth) c.errors.push_back(name + ": leaf at depth " + std::to_string(f.depth) + ", expected " + std::to_string(c.leaf_depth));
      return;
    }
    ++c.internals_seen;
    if (n->children.size() != n->keys.size() + 1) {
      c.errors.push_back(name + ": children/keys count mismatch");
      return;
    }
    if (is_root && n->keys.empty()) c.errors.push_back(name + ": internal root with no keys");
    if (!is_root && n->keys.size() < c.min_internal) {
      c.errors.push_back(name + ": underflow (" + std::to_string(n->keys.size()) + " < " + std::to_string(c.min_internal) + ")");
    }
    for (std::size_t i = 0; i < n->children.size(); ++i) {
      std::optional<Key> lo = i == 0 ? f.lo : std::optional<Key>(n->keys[i - 1]);
      std::optional<Key> hi = i == n->keys.size() ? f.hi : std::optional<Key>(n->keys[i]);
      check({n->children[i].get(), f.depth + 1, lo, hi});
    }
  };
  check({root_.get(), 0, std::nullopt, std::nullopt});

  // Leaf chain must visit exactly the leaves found by the tree walk, in the same order.
  const Node* walker = leftmost_leaf();
  std::size_t idx = 0;
  std::optional<Key> prev;
  while (walker) {
    if (idx >= leaves_in_order.size() || leaves_in_order[idx] != walker) {
      c.errors.push_back("leaf chain differs from tree order at leaf " + std::to_string(walker->id));
      break;
    }
    for (Key k : walker->keys) {
      if (prev && !(*prev < k)) c.errors.push_back("keys not ascending across leaf chain at key " + std::to_string(k));
      prev = k;
    }
    walker = walker->next;
    ++idx;
  }
  if (idx != leaves_in_order.size() && c.errors.empty()) c.errors.push_back("leaf chain is shorter than the set of leaves");

  if (c.keys_seen != size_) c.errors.push_back("key count " + std::to_string(c.keys_seen) + " != recorded size " + std::to_string(size_));
  if (c.leaves_seen != leaf_count_) c.errors.push_back("leaf count " + std::to_string(c.leaves_seen) + " != recorded " + std::to_string(leaf_count_));
  if (c.internals_seen != internal_count_) c.errors.push_back("internal count " + std::to_string(c.internals_seen) + " != recorded " + std::to_string(internal_count_));
  return c.errors;
}

}  // namespace indexcraft
