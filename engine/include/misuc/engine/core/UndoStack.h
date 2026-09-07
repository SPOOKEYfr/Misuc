#pragma once

#include <cstddef>
#include <vector>

namespace misuc::core {

// Snapshot-based undo/redo history for a copyable document type T.
//
// The stack stores a bounded list of snapshots with a cursor. push() appends a
// new snapshot (discarding any redo tail) and moves the cursor to it; undo()
// / redo() step the cursor back and forth. Intended for small documents like
// Session; not for per-keystroke histories of large projects.
template <typename T>
class UndoStack {
public:
  explicit UndoStack(std::size_t capacity = 50) : capacity_(capacity) {}

  // Record a new document state. O(capacity) when the history is full.
  void push(const T& state) {
    if (capacity_ == 0)
      return;
    if (index_ + 1 < history_.size())
      history_.erase(history_.begin() + index_ + 1, history_.end());
    history_.push_back(state);
    if (history_.size() > capacity_) {
      const std::size_t excess = history_.size() - capacity_;
      history_.erase(history_.begin(), history_.begin() + excess);
      index_ -= excess;
    }
    index_ = history_.size() - 1;
  }

  // Move the cursor one step back. Returns false when there is nothing to undo.
  bool undo() {
    if (index_ == 0 || history_.empty())
      return false;
    --index_;
    return true;
  }

  // Move the cursor one step forward. Returns false when nothing to redo.
  bool redo() {
    if (index_ + 1 >= history_.size())
      return false;
    ++index_;
    return true;
  }

  const T& state() const { return history_[index_]; }
  T& state() { return history_[index_]; }

  bool canUndo() const { return !history_.empty() && index_ > 0; }
  bool canRedo() const { return index_ + 1 < history_.size(); }

  std::size_t depth() const { return history_.size(); }
  void clear() {
    history_.clear();
    index_ = 0;
  }

private:
  std::size_t capacity_;
  std::vector<T> history_;
  std::size_t index_ = 0;
};

} // namespace misuc::core