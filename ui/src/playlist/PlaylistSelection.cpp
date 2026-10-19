#include "PlaylistSelection.h"

#include <algorithm>

namespace misuc::ui {

void PlaylistSelection::clear() {
  if (!items_.empty()) {
    items_.clear();
    anchor_.trackId.clear();
    anchor_.clipId.clear();
    anchor_.regionId.clear();
    fire();
  }
}

bool PlaylistSelection::contains(const Item& it) const {
  for (const auto& i : items_) {
    if (i == it) return true;
  }
  return false;
}

void PlaylistSelection::setSingle(const Item& it) {
  if (items_.size() == 1 && items_[0] == it) {
    anchor_ = it;
    return;
  }
  items_ = {it};
  anchor_ = it;
  fire();
}

void PlaylistSelection::toggle(const Item& it) {
  if (it.empty()) return;
  auto itPos = std::find(items_.begin(), items_.end(), it);
  if (itPos != items_.end()) {
    items_.erase(itPos);
    if (anchor_ == it) {
      anchor_ = items_.empty() ? Item{} : items_.front();
    }
  } else {
    items_.push_back(it);
    if (!hasAnchor()) anchor_ = it;
  }
  fire();
}

void PlaylistSelection::add(const Item& it) {
  if (it.empty()) return;
  if (!contains(it)) {
    items_.push_back(it);
    if (!hasAnchor()) anchor_ = it;
    fire();
  }
}

void PlaylistSelection::selectItems(const std::vector<Item>& newItems) {
  items_ = newItems;
  anchor_ = newItems.empty() ? Item{} : newItems.front();
  fire();
}

void PlaylistSelection::selectAll() {
  if (!session_) return;
  std::vector<Item> all;
  for (size_t row = 0; row < session_->tracks().size(); ++row) {
    const auto& t = session_->tracks()[row];
    for (const auto& c : t.clips) {
      all.push_back({t.id, c.id, {}});
    }
    for (const auto& r : t.regions) {
      all.push_back({t.id, {}, r.id});
    }
  }
  items_ = std::move(all);
  anchor_ = items_.empty() ? Item{} : items_.front();
  fire();
}

namespace {
struct SortedItem {
  int row;
  double startBeats;
  PlaylistSelection::Item item;
};
} // namespace

void PlaylistSelection::rangeTo(const Item& to, bool additive) {
  if (!session_ || items_.empty() || to.empty()) return;

  std::vector<SortedItem> all;
  for (size_t row = 0; row < session_->tracks().size(); ++row) {
    const auto& t = session_->tracks()[row];
    for (const auto& c : t.clips) {
      all.push_back({static_cast<int>(row), c.startBeats, {t.id, c.id, {}}});
    }
    for (const auto& r : t.regions) {
      all.push_back({static_cast<int>(row), r.startBeats, {t.id, {}, r.id}});
    }
  }
  std::sort(all.begin(), all.end(), [](const SortedItem& a, const SortedItem& b) {
    if (a.row != b.row) return a.row < b.row;
    return a.startBeats < b.startBeats;
  });

  int anchorIdx = -1, targetIdx = -1;
  for (int i = 0; i < (int)all.size(); ++i) {
    if (all[i].item == anchor_) anchorIdx = i;
    if (all[i].item == to) targetIdx = i;
  }
  if (anchorIdx < 0 || targetIdx < 0) return;

  int lo = std::min(anchorIdx, targetIdx);
  int hi = std::max(anchorIdx, targetIdx);

  std::vector<Item> rangeItems;
  for (int i = lo; i <= hi; ++i) rangeItems.push_back(all[i].item);

  if (additive) {
    for (const auto& it : rangeItems) add(it);
  } else {
    items_ = std::move(rangeItems);
    fire();
  }
}

PlaylistSelection::Item PlaylistSelection::makeItem(int row, int clipIdx, int regionIdx) const {
  Item it;
  if (!session_ || row < 0 || row >= (int)session_->tracks().size()) return it;
  const auto& t = session_->tracks()[static_cast<size_t>(row)];
  it.trackId = t.id;
  if (clipIdx >= 0 && clipIdx < (int)t.clips.size()) {
    it.clipId = t.clips[static_cast<size_t>(clipIdx)].id;
  } else if (regionIdx >= 0 && regionIdx < (int)t.regions.size()) {
    it.regionId = t.regions[static_cast<size_t>(regionIdx)].id;
  }
  return it;
}

std::vector<std::string> PlaylistSelection::selectedTrackIds() const {
  std::vector<std::string> ids;
  for (const auto& it : items_) {
    if (!it.trackId.empty()) {
      auto pos = std::find(ids.begin(), ids.end(), it.trackId);
      if (pos == ids.end()) ids.push_back(it.trackId);
    }
  }
  return ids;
}

} // namespace misuc::ui