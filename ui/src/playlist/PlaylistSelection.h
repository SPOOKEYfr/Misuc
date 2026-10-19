#pragma once

#include <misuc/engine/project/Session.h>
#include <vector>
#include <string>
#include <functional>

namespace misuc::ui {

class PlaylistSelection {
public:
  struct Item {
    std::string trackId;
    std::string clipId;
    std::string regionId;

    bool isMidi() const { return !regionId.empty(); }
    bool isClip() const { return !clipId.empty(); }
    bool empty() const { return clipId.empty() && regionId.empty(); }

    bool operator==(const Item& other) const {
      return trackId == other.trackId && clipId == other.clipId && regionId == other.regionId;
    }
    bool operator!=(const Item& other) const { return !(*this == other); }
  };

  using ChangedFn = std::function<void()>;

  PlaylistSelection() = default;

  void setSession(misuc::project::Session* s) { session_ = s; }
  void setChangedFn(ChangedFn fn) { onChanged_ = std::move(fn); }

  void clear();
  bool empty() const { return items_.empty(); }
  int count() const { return static_cast<int>(items_.size()); }
  const std::vector<Item>& items() const { return items_; }

  bool contains(const Item& it) const;
  void setSingle(const Item& it);
  void toggle(const Item& it);
  void add(const Item& it);
  void selectItems(const std::vector<Item>& newItems);
  void selectAll();
  void rangeTo(const Item& to, bool additive);

  const Item& anchor() const { return anchor_; }
  void setAnchor(const Item& it) { anchor_ = it; }
  bool hasAnchor() const { return !anchor_.empty(); }

  // Resolve index-based click to Item (uses session for stable IDs).
  Item makeItem(int row, int clipIdx, int regionIdx) const;

  // Helpers for rendering
  std::vector<std::string> selectedTrackIds() const;

private:
  void fire() { if (onChanged_) onChanged_(); }

  misuc::project::Session* session_ = nullptr;
  std::vector<Item> items_;
  Item anchor_;
  ChangedFn onChanged_;
};

} // namespace misuc::ui