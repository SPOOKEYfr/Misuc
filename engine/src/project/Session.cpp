#include <misuc/engine/project/Session.h>

#include <algorithm>
#include <stdexcept>
#include <string>

#include <misuc/engine/core/ProjectFile.h>

namespace misuc::project {

using core::Json;

namespace {
// Grams an fx automation lane param ("fx:<chain>:<port>") into its indices.
bool parseFxLaneParam(const std::string& param, uint32_t& chain,
                      uint32_t& port) {
  if (param.size() < 3 || param.compare(0, 3, "fx:") != 0)
    return false;
  const size_t sep = param.find(':', 3);
  if (sep == std::string::npos || sep == 3 || sep + 1 >= param.size())
    return false;
  auto toU32 = [](const std::string& s, uint32_t& dst) {
    if (s.empty() || s.size() > 9)
      return false;
    for (char c : s)
      if (c < '0' || c > '9')
        return false;
    dst = static_cast<uint32_t>(std::stoul(s));
    return true;
  };
  if (!toU32(param.substr(3, sep - 3), chain))
    return false;
  if (!toU32(param.substr(sep + 1), port))
    return false;
  return true;
}
}  // namespace

Session::Session() = default;

bool Session::ensureCreatedUtc() {
  if (!createdUtc_.empty())
    return false;
  createdUtc_ = core::utcNowIso();
  return true;
}

void Session::touchModified() { modifiedUtc_ = core::utcNowIso(); }

double Session::contentEndBeats() const {
  double end = 0.0;
  for (const Track& t : tracks_) {
    for (const AudioClip& c : t.clips)
      end = std::max(end, c.startBeats + c.durationBeats);
    if (!t.regions.empty()) {
      for (const MidiRegion& r : t.regions) {
        double rEnd = r.startBeats + r.durationBeats;
        for (const NoteEvent& n : r.notes)
          rEnd = std::max(rEnd, r.startBeats + n.startBeats + n.durationBeats);
        end = std::max(end, rEnd);
      }
    } else {
      // Legacy whole-track notes play back directly at absolute beats.
      for (const NoteEvent& n : t.notes)
        end = std::max(end, n.startBeats + n.durationBeats);
    }
  }
  return end;
}

std::string Session::newTrackId() {
  return "track-" + std::to_string(++trackCounter_);
}

std::string Session::newClipId() {
  return "clip-" + std::to_string(++clipCounter_);
}

std::string Session::newTakeId() {
  return "take-" + std::to_string(++takeCounter_);
}

std::string Session::addTake(const std::string& trackId, RecordTake take) {
  Track* t = findTrack(trackId);
  if (!t)
    return std::string();
  take.id = newTakeId();
  t->takes.push_back(std::move(take));
  return t->takes.back().id;
}

const std::vector<RecordTake>& Session::takes(const std::string& trackId) const {
  const Track* t = findTrack(trackId);
  static const std::vector<RecordTake> kEmpty;
  return t ? t->takes : kEmpty;
}

RecordTake* Session::findTake(const std::string& trackId,
                              const std::string& takeId) {
  Track* t = findTrack(trackId);
  if (!t)
    return nullptr;
  for (RecordTake& k : t->takes)
    if (k.id == takeId)
      return &k;
  return nullptr;
}

const RecordTake* Session::findTake(const std::string& trackId,
                                    const std::string& takeId) const {
  const Track* t = findTrack(trackId);
  if (!t)
    return nullptr;
  for (const RecordTake& k : t->takes)
    if (k.id == takeId)
      return &k;
  return nullptr;
}

bool Session::removeTake(const std::string& trackId,
                         const std::string& takeId) {
  Track* t = findTrack(trackId);
  if (!t)
    return false;
  for (auto it = t->takes.begin(); it != t->takes.end(); ++it) {
    if (it->id == takeId) {
      t->takes.erase(it);
      return true;
    }
  }
  return false;
}

bool Session::setRecordArm(const std::string& trackId, bool on) {
  Track* t = findTrack(trackId);
  if (!t)
    return false;
  t->recordArm = on;
  return true;
}

bool Session::isRecordArmed(const std::string& trackId) const {
  const Track* t = findTrack(trackId);
  return t && t->recordArm;
}

static const uint32_t kTrackColorPalette[] = {
    0xFF8F7BFA, // theme::kAccent (restrained violet)
    0xFF4ADE80, // theme::kOk (green)
    0xFFF0AD4E, // amber
    0xFFD9534F, // red
    0xFF5BC0DE, // cyan
    0xFFF7464A, // orange-red
};

std::string Session::addTrack(std::string name) {
  Track t;
  t.id = newTrackId();
  t.name = std::move(name);
  t.color = kTrackColorPalette[(trackCounter_ - 1) % (sizeof(kTrackColorPalette) / sizeof(kTrackColorPalette[0]))];
  tracks_.push_back(std::move(t));
  return tracks_.back().id;
}

bool Session::removeTrack(const std::string& id) {
  for (auto it = tracks_.begin(); it != tracks_.end(); ++it) {
    if (it->id == id) {
      tracks_.erase(it);
      return true;
    }
  }
  return false;
}

Track* Session::findTrack(const std::string& id) {
  for (Track& t : tracks_)
    if (t.id == id)
      return &t;
  return nullptr;
}

const Track* Session::findTrack(const std::string& id) const {
  for (const Track& t : tracks_)
    if (t.id == id)
      return &t;
  return nullptr;
}

std::string Session::addClip(const std::string& trackId, AudioClip clip) {
  Track* t = findTrack(trackId);
  if (!t)
    return std::string();
  clip.id = newClipId();
  t->clips.push_back(std::move(clip));
  return t->clips.back().id;
}

std::string Session::newRegionId() {
  return "region-" + std::to_string(++regionCounter_);
}

std::string Session::newAutomationId() {
  return "automation-" + std::to_string(++automationCounter_);
}

std::string Session::newBusId() {
  return "bus-" + std::to_string(++busCounter_);
}

std::string Session::newFxId() {
  return "fx-" + std::to_string(++fxCounter_);
}

std::string Session::newSendId() {
  return "send-" + std::to_string(++sendCounter_);
}

std::string Session::newChannelId() {
  return "ch" + std::to_string(++channelCounter_);
}

std::string Session::addFx(const std::string& trackId, std::string uri) {
  Track* t = findTrack(trackId);
  if (!t)
    return std::string();
  TrackFx f;
  f.id = newFxId();
  f.uri = std::move(uri);
  t->fx.push_back(std::move(f));
  return t->fx.back().id;
}

bool Session::removeFx(const std::string& trackId, const std::string& fxId) {
  Track* t = findTrack(trackId);
  if (!t)
    return false;
  for (auto it = t->fx.begin(); it != t->fx.end(); ++it) {
    if (it->id != fxId)
      continue;
    const size_t removed = static_cast<size_t>(it - t->fx.begin());
    t->fx.erase(it);
    // FX control-port automation lanes ("fx:<chain>:<port>") address inserts
    // by chain position; keep them consistent after the slot disappears: drop
    // lanes that targeted the removed insert and shift lanes that pointed at
    // later inserts back by one so they keep automating the same plugin.
    auto laneIt = t->automation.begin();
    while (laneIt != t->automation.end()) {
      uint32_t ci = 0, pi = 0;
      if (!parseFxLaneParam(laneIt->param, ci, pi)) {
        ++laneIt;
        continue;
      }
      if (ci == removed) {
        laneIt = t->automation.erase(laneIt);
        continue;
      }
      if (ci > removed)
        laneIt->param = "fx:" + std::to_string(ci - 1) + ":" + std::to_string(pi);
      ++laneIt;
    }
    return true;
  }
  return false;
}

std::string Session::addBus(std::string name) {
  Bus b;
  b.id = newBusId();
  b.name = std::move(name);
  buses_.push_back(std::move(b));
  return buses_.back().id;
}

bool Session::removeBus(const std::string& busId) {
  for (auto it = buses_.begin(); it != buses_.end(); ++it) {
    if (it->id != busId)
      continue;
    buses_.erase(it);
    // Anything routed into the removed bus falls back to the master output.
    for (Track& t : tracks_)
      if (t.busId == busId)
        t.busId.clear();
    // Buses that routed into it fall back to the master output too.
    for (Bus& b : buses_)
      if (b.target == busId)
        b.target.clear();
    // Sends that used the bus as a target or source are dropped with it.
    for (auto s = sends_.begin(); s != sends_.end();) {
      if (s->targetBus == busId || s->sourceBus == busId)
        s = sends_.erase(s);
      else
        ++s;
    }
    return true;
  }
  return false;
}

Bus* Session::findBus(const std::string& busId) {
  for (Bus& b : buses_)
    if (b.id == busId)
      return &b;
  return nullptr;
}

const Bus* Session::findBus(const std::string& busId) const {
  for (const Bus& b : buses_)
    if (b.id == busId)
      return &b;
  return nullptr;
}

// A bus routes into another bus unless this would form a cycle (a bus feeding
// itself, directly or transitively). Cycles are rejected here — the model never
// holds one — so the engine's graph renderer only ever sees a DAG.
bool Session::setBusTarget(const std::string& busId,
                           const std::string& targetBusId) {
  Bus* b = findBus(busId);
  if (!b)
    return false;
  if (targetBusId.empty() || targetBusId == busId) {
    if (targetBusId == busId)
      return false;
    b->target.clear();
    return true;
  }
  if (!findBus(targetBusId))
    return false;
  // Would `busId -> targetBusId` create a cycle? Walk up from the target.
  // A solo bus -- no cycle detection needed.
  if (busId == targetBusId)
    return false;
  std::string cur = targetBusId;
  while (!cur.empty()) {
    const Bus* c = findBus(cur);
    if (!c)
      break;
    if (c->target == busId)
      return false; // target already feeds back into us -> cycle
    cur = c->target;
  }
  b->target = targetBusId;
  return true;
}

std::string Session::addSend(const std::string& sourceTrackId,
                             const std::string& sourceBusId,
                             const std::string& targetBusId, float levelDb) {
  if (!findBus(targetBusId))
    return std::string();
  // Exactly one source: a track or a bus (a bus-sourced send duplicates that
  // bus's full post-fx output at the given level).
  const bool hasTrack = !sourceTrackId.empty() && findTrack(sourceTrackId);
  const bool hasBus = !sourceBusId.empty() && findBus(sourceBusId);
  if (!hasTrack && !hasBus)
    return std::string();
  Send s;
  s.id = newSendId();
  if (hasTrack)
    s.sourceTrack = sourceTrackId;
  else
    s.sourceBus = sourceBusId;
  s.targetBus = targetBusId;
  s.levelDb = std::clamp(levelDb, -60.f, 12.f);
  sends_.push_back(std::move(s));
  return sends_.back().id;
}

bool Session::removeSend(const std::string& sendId) {
  for (auto it = sends_.begin(); it != sends_.end(); ++it) {
    if (it->id == sendId) {
      sends_.erase(it);
      return true;
    }
  }
  return false;
}

Send* Session::findSend(const std::string& sendId) {
  for (Send& s : sends_)
    if (s.id == sendId)
      return &s;
  return nullptr;
}

const Send* Session::findSend(const std::string& sendId) const {
  for (const Send& s : sends_)
    if (s.id == sendId)
      return &s;
  return nullptr;
}

std::string Session::addBusFx(const std::string& busId, std::string uri,
                              std::string sidechainSource) {
  Bus* b = findBus(busId);
  if (!b)
    return std::string();
  TrackFx f;
  f.id = newFxId();
  f.uri = std::move(uri);
  f.sidechainSource = std::move(sidechainSource);
  b->fx.push_back(std::move(f));
  return b->fx.back().id;
}

bool Session::removeBusFx(const std::string& busId, const std::string& fxId) {
  Bus* b = findBus(busId);
  if (!b)
    return false;
  for (auto it = b->fx.begin(); it != b->fx.end(); ++it) {
    if (it->id == fxId) {
      b->fx.erase(it);
      return true;
    }
  }
  return false;
}

std::string Session::addAutomationLane(const std::string& trackId,
                                       const std::string& param) {
  // The two-argument form picks the param's resting value: pan automates the
  // L/R position (centre 0.0); gain/volume automate amplitude (unity 1.0).
  // FX "fx:<chain>:<port>" lanes have no universal neutral, so they must use
  // the resting-value overload (the control's current value).
  const double resting = param == "pan" ? 0.0 : 1.0;
  return addAutomationLane(trackId, param, resting);
}

std::string Session::addAutomationLane(const std::string& trackId,
                                       const std::string& param,
                                       double restingValue) {
  Track* t = findTrack(trackId);
  if (!t)
    return std::string();
  AutomationLane lane;
  lane.id = newAutomationId();
  lane.param = param;
  // Two endpoints at the resting value so an edit always has a visible curve.
  const double end = std::max(contentEndBeats(), 4.0);
  lane.points.push_back({0.0, restingValue});
  lane.points.push_back({end, restingValue});
  t->automation.push_back(std::move(lane));
  return t->automation.back().id;
}

AutomationLane* Session::findAutomationLane(const std::string& trackId,
                                            const std::string& laneId) {
  Track* t = findTrack(trackId);
  if (!t)
    return nullptr;
  for (auto& l : t->automation)
    if (l.id == laneId)
      return &l;
  return nullptr;
}

const AutomationLane* Session::findAutomationLane(const std::string& trackId,
                                                  const std::string& laneId) const {
  const Track* t = findTrack(trackId);
  if (!t)
    return nullptr;
  for (const auto& l : t->automation)
    if (l.id == laneId)
      return &l;
  return nullptr;
}

bool Session::removeAutomationLane(const std::string& trackId,
                                   const std::string& laneId) {
  Track* t = findTrack(trackId);
  if (!t)
    return false;
  auto& v = t->automation;
  const auto it = std::find_if(v.begin(), v.end(),
                               [&](const AutomationLane& l) { return l.id == laneId; });
  if (it == v.end())
    return false;
  v.erase(it);
  return true;
}

std::string Session::addRegion(const std::string& trackId, std::string name,
                               double startBeats, double durationBeats) {
  Track* t = findTrack(trackId);
  if (!t)
    return std::string();
  MidiRegion r;
  r.id = newRegionId();
  r.name = std::move(name);
  r.startBeats = startBeats;
  r.durationBeats = durationBeats > 0.0 ? durationBeats : 1.0;
  t->regions.push_back(std::move(r));
  return t->regions.back().id;
}

std::vector<NoteEvent> rackPatternNotes(const Session& session, int bank) {
  std::vector<NoteEvent> out;
  const ChannelRackData& rack = session.channelRack();
  const int b = bank >= 0 && bank < kPatternBanks ? bank : rack.activePattern;
  bool anySolo = false;
  for (const StepChannel& c : rack.channels)
    if (c.solo) {
      anySolo = true;
      break;
    }
  for (int row = 0; row < (int)rack.channels.size(); ++row) {
    const StepChannel& c = rack.channels[row];
    if (c.muted)
      continue;
    // Automation lanes make no sound of their own (master spec §5): they
    // route control data, so they never contribute step gates.
    if (c.type == ChannelType::Automation)
      continue;
    if (anySolo && !c.solo)
      continue;
    const int span = std::clamp(rack.stepsPerPattern, 4, kMaxStepCount);
    for (int step = 0; step < span; ++step) {
      if (!c.steps[b][step])
        continue;
      NoteEvent n;
      n.pitch = static_cast<uint8_t>(rackRowPitch(row));
      n.velocity = c.steps[b][step];   // per-step velocity
      n.startBeats = step * 0.25;   // 16th-note grid within the block
      n.durationBeats = 0.24;
      out.push_back(n);
    }
  }
  return out;
}

std::string materializePatternRegion(Session& session, const std::string& trackId,
                                     int bank, double startBeats,
                                     double durationBeats) {
  if (!session.findTrack(trackId))
    return std::string();
  const ChannelRackData& rack = session.channelRack();
  const int b = bank >= 0 && bank < kPatternBanks ? bank : rack.activePattern;
  const std::string rid = session.addRegion(
      trackId, "Pattern " + std::to_string(b + 1), startBeats, durationBeats);
  if (rid.empty())
    return std::string();
  if (MidiRegion* r = session.findRegion(trackId, rid))
    r->notes = rackPatternNotes(session, b);
  return rid;
}

MidiRegion* Session::findRegion(const std::string& trackId,
                                const std::string& regionId) {
  Track* t = findTrack(trackId);
  if (!t)
    return nullptr;
  for (MidiRegion& r : t->regions)
    if (r.id == regionId)
      return &r;
  return nullptr;
}

const MidiRegion* Session::findRegion(const std::string& trackId,
                                      const std::string& regionId) const {
  const Track* t = findTrack(trackId);
  if (!t)
    return nullptr;
  for (const MidiRegion& r : t->regions)
    if (r.id == regionId)
      return &r;
  return nullptr;
}

bool Session::removeRegion(const std::string& trackId,
                           const std::string& regionId) {
  Track* t = findTrack(trackId);
  if (!t)
    return false;
  for (auto it = t->regions.begin(); it != t->regions.end(); ++it) {
    if (it->id == regionId) {
      t->regions.erase(it);
      return true;
    }
  }
  return false;
}

std::string Session::duplicateRegion(const std::string& trackId,
                                     const std::string& regionId) {
  Track* t = findTrack(trackId);
  if (!t)
    return std::string();
  for (const MidiRegion& src : t->regions) {
    if (src.id != regionId)
      continue;
    MidiRegion copy = src;
    copy.id = newRegionId();
    copy.name += " Copy";
    copy.startBeats += copy.durationBeats;
    t->regions.push_back(std::move(copy));
    return t->regions.back().id;
  }
  return std::string();
}

std::string Session::toJson() const {
  Json::Object root;
  root.emplace("format", Json("misuc.song"));
  root.emplace("schemaVersion", Json(Json::kSchemaVersion));
  root.emplace("app", Json("misuc"));

  Json::Object meta;
  meta.emplace("name", Json(name_));
  meta.emplace("artist", Json(artist_));
  meta.emplace("createdUtc", Json(std::string(createdUtc_)));
  meta.emplace("modifiedUtc", Json(std::string(modifiedUtc_)));
  root.emplace("metadata", Json(std::move(meta)));

  Json::Object transportObj;
  transportObj.emplace("tempo", Json(tempo_));
  transportObj.emplace("timeSigNumerator", Json(tsNum_));
  transportObj.emplace("timeSigDenominator", Json(tsDen_));
  transportObj.emplace("sampleRate", Json(static_cast<double>(sampleRate_)));
  transportObj.emplace("loopMode", Json(loopMode_));
  transportObj.emplace("loopStartBeats", Json(loopStartBeats_));
  transportObj.emplace("loopEndBeats", Json(loopEndBeats_));
  transportObj.emplace("masterGainDb", Json(static_cast<double>(masterGainDb_)));
  transportObj.emplace("metronomeOn", Json(metronomeOn_));
  transportObj.emplace("metronomeVolume", Json(static_cast<double>(metronomeVolume_)));
  transportObj.emplace("countInBeats", Json(static_cast<double>(countInBeats_)));
  root.emplace("transport", Json(std::move(transportObj)));

  // FX chain serializer shared by tracks and buses (same "fx" shape).
  const auto fxToJson = [](const std::vector<TrackFx>& fx) -> Json {
    Json::Array arr;
    for (const TrackFx& f : fx) {
      Json::Object fj;
      fj.emplace("id", Json(f.id));
      fj.emplace("uri", Json(f.uri));
      fj.emplace("enabled", Json(f.enabled));
      if (!f.sidechainSource.empty())
        fj.emplace("sc", Json(f.sidechainSource));
      Json::Array ps;
      for (const FxParam& p : f.params) {
        Json::Object pj;
        pj.emplace("index", Json(static_cast<double>(p.index)));
        pj.emplace("value", Json(static_cast<double>(p.value)));
        ps.emplace_back(Json(std::move(pj)));
      }
      fj.emplace("params", Json(std::move(ps)));
      arr.emplace_back(Json(std::move(fj)));
    }
    return Json(std::move(arr));
  };

  Json::Array tracks;
  for (const Track& t : tracks_) {
    Json::Object tobj;
    tobj.emplace("id", Json(t.id));
    tobj.emplace("name", Json(t.name));
    if (t.color != 0)
      tobj.emplace("color", Json(static_cast<double>(t.color)));
    tobj.emplace("muted", Json(t.muted));
    tobj.emplace("solo", Json(t.solo));
    tobj.emplace("gainDb", Json(static_cast<double>(t.gainDb)));
    tobj.emplace("pan", Json(static_cast<double>(t.pan)));
    if (!t.busId.empty())
      tobj.emplace("busId", Json(t.busId));

    if (!t.fx.empty())
      tobj.emplace("fx", fxToJson(t.fx));

    Json::Array notes;
    for (const NoteEvent& n : t.notes) {
      Json::Object nobj;
      nobj.emplace("channel", Json(static_cast<double>(n.channel)));
      nobj.emplace("pitch", Json(static_cast<double>(n.pitch)));
      nobj.emplace("velocity", Json(static_cast<double>(n.velocity)));
      nobj.emplace("startBeats", Json(n.startBeats));
      nobj.emplace("durationBeats", Json(n.durationBeats));
      notes.emplace_back(Json(std::move(nobj)));
    }
    tobj.emplace("notes", Json(std::move(notes)));

    Json::Array regions;
    for (const MidiRegion& r : t.regions) {
      Json::Object rj;
      rj.emplace("id", Json(r.id));
      rj.emplace("name", Json(r.name));
      rj.emplace("startBeats", Json(r.startBeats));
      rj.emplace("durationBeats", Json(r.durationBeats));
      Json::Array rnotes;
      for (const NoteEvent& n : r.notes) {
        Json::Object nobj;
        nobj.emplace("channel", Json(static_cast<double>(n.channel)));
        nobj.emplace("pitch", Json(static_cast<double>(n.pitch)));
        nobj.emplace("velocity", Json(static_cast<double>(n.velocity)));
        nobj.emplace("startBeats", Json(n.startBeats));
        nobj.emplace("durationBeats", Json(n.durationBeats));
        rnotes.emplace_back(Json(std::move(nobj)));
      }
      rj.emplace("notes", Json(std::move(rnotes)));
      regions.emplace_back(Json(std::move(rj)));
    }
    tobj.emplace("regions", Json(std::move(regions)));

    Json::Array clips;
    for (const AudioClip& c : t.clips) {
      Json::Object cobj;
      cobj.emplace("id", Json(c.id));
      cobj.emplace("name", Json(c.name));
      cobj.emplace("sourceFile", Json(c.sourceFile));
      cobj.emplace("startBeats", Json(c.startBeats));
      cobj.emplace("durationBeats", Json(c.durationBeats));
      cobj.emplace("clipOffsetBeats", Json(c.clipOffsetBeats));
      cobj.emplace("gainDb", Json(static_cast<double>(c.gainDb)));
      cobj.emplace("fadeInBeats", Json(c.fadeInBeats));
      cobj.emplace("fadeOutBeats", Json(c.fadeOutBeats));
      cobj.emplace("sampleRate", Json(static_cast<double>(c.sampleRate)));
      cobj.emplace("channels", Json(static_cast<double>(c.channels)));
      clips.emplace_back(Json(std::move(cobj)));
    }
    tobj.emplace("clips", Json(std::move(clips)));

    if (t.recordArm)
      tobj.emplace("recordArm", Json(true));

    if (!t.takes.empty()) {
      Json::Array takes;
      for (const RecordTake& k : t.takes) {
        Json::Object kj;
        kj.emplace("id", Json(k.id));
        kj.emplace("name", Json(k.name));
        kj.emplace("sourceFile", Json(k.sourceFile));
        kj.emplace("startBeats", Json(k.startBeats));
        kj.emplace("durationBeats", Json(k.durationBeats));
        kj.emplace("gainDb", Json(static_cast<double>(k.gainDb)));
        kj.emplace("sampleRate", Json(static_cast<double>(k.sampleRate)));
        kj.emplace("channels", Json(static_cast<double>(k.channels)));
        if (!k.clipId.empty())
          kj.emplace("clipId", Json(k.clipId));
        takes.emplace_back(Json(std::move(kj)));
      }
      tobj.emplace("takes", Json(std::move(takes)));
    }

    Json::Array automation;
    for (const AutomationLane& l : t.automation) {
      Json::Object aj;
      aj.emplace("id", Json(l.id));
      aj.emplace("param", Json(l.param));
      aj.emplace("muted", Json(l.muted));
      aj.emplace("armed", Json(l.armed));
      Json::Array pts;
      for (const AutomationPoint& p : l.points) {
        Json::Object pj;
        pj.emplace("timeBeats", Json(p.timeBeats));
        pj.emplace("value", Json(p.value));
        pts.emplace_back(Json(std::move(pj)));
      }
      aj.emplace("points", Json(std::move(pts)));
      automation.emplace_back(Json(std::move(aj)));
    }
    tobj.emplace("automation", Json(std::move(automation)));
    tracks.emplace_back(Json(std::move(tobj)));
  }
  root.emplace("tracks", Json(std::move(tracks)));

  Json::Array buses;
  for (const Bus& b : buses_) {
    Json::Object bj;
    bj.emplace("id", Json(b.id));
    bj.emplace("name", Json(b.name));
    bj.emplace("gainDb", Json(static_cast<double>(b.faderDb)));
    bj.emplace("pan", Json(static_cast<double>(b.pan)));
    bj.emplace("muted", Json(b.muted));
    bj.emplace("solo", Json(b.solo));
    if (!b.target.empty())
      bj.emplace("target", Json(b.target));
    if (!b.fx.empty())
      bj.emplace("fx", fxToJson(b.fx));
    buses.emplace_back(Json(std::move(bj)));
  }
  root.emplace("buses", Json(std::move(buses)));

  // Parallel sends (Comp19).
  if (!sends_.empty()) {
    Json::Array sends;
    for (const Send& s : sends_) {
      Json::Object so;
      so.emplace("id", Json(s.id));
      if (!s.sourceTrack.empty())
        so.emplace("sourceTrack", Json(s.sourceTrack));
      if (!s.sourceBus.empty())
        so.emplace("sourceBus", Json(s.sourceBus));
      so.emplace("targetBus", Json(s.targetBus));
      so.emplace("levelDb", Json(static_cast<double>(s.levelDb)));
      so.emplace("enabled", Json(s.enabled));
      sends.emplace_back(Json(std::move(so)));
    }
    root.emplace("sends", Json(std::move(sends)));
  }

  // Channel rack (step sequencer) ----------------------------------------
  Json::Object crObj;
  crObj.emplace("activePattern", Json(static_cast<double>(channelRack_.activePattern)));
  crObj.emplace("swing", Json(static_cast<double>(channelRack_.swing)));
  // Pattern length in steps (4..128). Additive: legacy documents without the
  // key load with the 16-step default (master spec §9: not hardcoded to 16).
  const int sp = std::clamp(channelRack_.stepsPerPattern, 4, kMaxStepCount);
  crObj.emplace("stepsPerPattern", Json(static_cast<double>(sp)));
  Json::Array crChannels;
  uint32_t fallback = 0;
  for (const StepChannel& ch : channelRack_.channels) {
    Json::Object chObj;
    // Every channel carries a stable identity (master spec §4). Creators
    // allocate one up-front; this path can only see an empty id for hand-built
    // channels, so it emits a positional placeholder the loader absorbs.
    chObj.emplace("id", Json(ch.id.empty()
                                 ? "ch" + std::to_string(++fallback)
                                 : ch.id));
    chObj.emplace("name", Json(ch.name));
    chObj.emplace("type", Json(channelTypeName(ch.type)));
    chObj.emplace("muted", Json(ch.muted));
    chObj.emplace("solo", Json(ch.solo));
    chObj.emplace("volume", Json(static_cast<double>(ch.volume)));
    chObj.emplace("pan", Json(static_cast<double>(ch.pan)));
    if (!ch.soundFile.empty())
      chObj.emplace("sound", Json(ch.soundFile));
    for (int b = 0; b < kPatternBanks; ++b) {
      // Encode the active portion of each lane as a compact byte array
      // (base64). Each byte is the step's velocity (0 = off, 1..127 = on
      // with velocity).
      std::string st(static_cast<size_t>(sp), '\0');
      for (int i = 0; i < sp; ++i)
        st[static_cast<size_t>(i)] = static_cast<char>(ch.steps[b][i]);
      // Base64 encode manually (no Qt dependency in the engine layer).
      static constexpr char kTbl[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string b64;
      b64.reserve(((kStepCount + 2) / 3) * 4);
      for (size_t i = 0; i < st.size(); i += 3) {
        unsigned n = static_cast<unsigned>(static_cast<unsigned char>(st[i])) << 16;
        if (i + 1 < st.size())
          n |= static_cast<unsigned>(static_cast<unsigned char>(st[i + 1])) << 8;
        if (i + 2 < st.size())
          n |= static_cast<unsigned>(static_cast<unsigned char>(st[i + 2]));
        b64 += kTbl[(n >> 18) & 0x3F];
        b64 += kTbl[(n >> 12) & 0x3F];
        b64 += (i + 1 < st.size()) ? kTbl[(n >> 6) & 0x3F] : '=';
        b64 += (i + 2 < st.size()) ? kTbl[n & 0x3F] : '=';
      }
      chObj.emplace("steps" + std::to_string(b), Json(std::move(b64)));
    }
    crChannels.emplace_back(Json(std::move(chObj)));
  }
  crObj.emplace("channels", Json(std::move(crChannels)));
  root.emplace("channelRack", Json(std::move(crObj)));

  return Json(std::move(root)).dump();
}

Session Session::fromJson(const std::string& text) {
  const Json doc = Json::parse(text);
  if (!doc.isObject())
    throw std::runtime_error("Project root must be a JSON object");

  Session s;
  const auto& root = doc.object();

  // Format gate: a future non-song document claiming another format must not
  // be silently loaded as a session. Legacy documents without the marker are
  // still accepted (they are all songs).
  if (auto f = root.find("format"); f != root.end()) {
    if (!f->second.isString() || f->second.string() != "misuc.song")
      throw std::runtime_error("Unsupported project format marker");
  }

  // Schema version gate: the loader is tolerant of older versions (v1's flat
  // notes migrate to a region, v2 fields read as-is) but must refuse future
  // documents rather than guess at layout.
  int version = 1;
  if (auto v = root.find("schemaVersion"); v != root.end() &&
                                           v->second.isNumber())
    version = static_cast<int>(v->second.number());
  if (version > Json::kSchemaVersion) {
    throw std::runtime_error(
        "Unsupported project schema version: " + std::to_string(version) +
        " (this build reads up to version " +
        std::to_string(Json::kSchemaVersion) + ")");
  }

  auto it = root.find("transport");
  if (it != root.end() && it->second.isObject()) {
    const auto& t = it->second.object();
    if (auto f = t.find("tempo"); f != t.end() && f->second.isNumber())
      s.setTempo(f->second.number());
    if (auto f = t.find("timeSigNumerator"); f != t.end() && f->second.isNumber())
      s.setTimeSignature(static_cast<int>(f->second.number()), s.timeSigDenominator());
    if (auto f = t.find("timeSigDenominator"); f != t.end() && f->second.isNumber())
      s.setTimeSignature(s.timeSigNumerator(), static_cast<int>(f->second.number()));
    if (auto f = t.find("sampleRate"); f != t.end() && f->second.isNumber())
      s.setSampleRate(static_cast<uint32_t>(f->second.number()));
    bool loopMode = false;
    double ls = 0.0;
    double le = 4.0;
    if (auto f = t.find("loopMode"); f != t.end() && f->second.isBool())
      loopMode = f->second.boolean();
    if (auto f = t.find("loopStartBeats"); f != t.end() && f->second.isNumber())
      ls = f->second.number();
    if (auto f = t.find("loopEndBeats"); f != t.end() && f->second.isNumber())
      le = f->second.number();
    s.setLoop(loopMode, ls, le);
    if (auto f = t.find("masterGainDb"); f != t.end() && f->second.isNumber())
      s.setMasterGainDb(static_cast<float>(f->second.number()));
    // v4 metronome fields: additive so old docs load with defaults.
    if (auto f = t.find("metronomeOn"); f != t.end() && f->second.isBool())
      s.setMetronomeOn(f->second.boolean());
    if (auto f = t.find("metronomeVolume"); f != t.end() && f->second.isNumber())
      s.setMetronomeVolume(static_cast<float>(f->second.number()));
    if (auto f = t.find("countInBeats"); f != t.end() && f->second.isNumber())
      s.setCountInBeats(static_cast<float>(f->second.number()));
  }

  if (auto f = root.find("metadata"); f != root.end() && f->second.isObject()) {
    const auto& m = f->second.object();
    if (auto n = m.find("name"); n != m.end() && n->second.isString())
      s.setName(n->second.string());
    if (auto n = m.find("artist"); n != m.end() && n->second.isString())
      s.setArtist(n->second.string());
    if (auto n = m.find("createdUtc"); n != m.end() && n->second.isString())
      s.createdUtc_ = n->second.string();
    if (auto n = m.find("modifiedUtc"); n != m.end() && n->second.isString())
      s.modifiedUtc_ = n->second.string();
  }

  if (auto f = root.find("tracks"); f != root.end() && f->second.isArray()) {
    for (const Json& tj : f->second.array()) {
      if (!tj.isObject())
        continue;
      const auto& to = tj.object();
      Track t;
      if (auto v = to.find("id"); v != to.end() && v->second.isString())
        t.id = v->second.string();
      else
        t.id = s.newTrackId();
      if (auto v = to.find("name"); v != to.end() && v->second.isString())
        t.name = v->second.string();
      if (auto v = to.find("color"); v != to.end() && v->second.isNumber())
        t.color = static_cast<uint32_t>(v->second.number());
      if (auto v = to.find("muted"); v != to.end() && v->second.isBool())
        t.muted = v->second.boolean();
      if (auto v = to.find("solo"); v != to.end() && v->second.isBool())
        t.solo = v->second.boolean();
      if (auto v = to.find("gainDb"); v != to.end() && v->second.isNumber())
        t.gainDb = static_cast<float>(v->second.number());
      if (auto v = to.find("pan"); v != to.end() && v->second.isNumber())
        t.pan = static_cast<float>(v->second.number());
      if (auto v = to.find("busId"); v != to.end() && v->second.isString())
        t.busId = v->second.string();

      if (auto v = to.find("fx"); v != to.end() && v->second.isArray()) {
        for (const Json& fj : v->second.array()) {
          if (!fj.isObject())
            continue;
          const auto& fo = fj.object();
          TrackFx f;
          if (auto x = fo.find("id"); x != fo.end() && x->second.isString())
            f.id = x->second.string();
          else
            f.id = s.newFxId();
          if (auto x = fo.find("uri"); x != fo.end() && x->second.isString())
            f.uri = x->second.string();
          if (auto x = fo.find("enabled"); x != fo.end() && x->second.isBool())
            f.enabled = x->second.boolean();
if (auto x = fo.find("params");
          x != fo.end() && x->second.isArray()) {
        for (const Json& pj : x->second.array()) {
          if (!pj.isObject())
            continue;
          const auto& po = pj.object();
          FxParam p;
          if (auto y = po.find("index");
              y != po.end() && y->second.isNumber())
            p.index = static_cast<uint32_t>(y->second.number());
          if (auto y = po.find("value");
              y != po.end() && y->second.isNumber())
            p.value = static_cast<float>(y->second.number());
          f.params.push_back(p);
        }
      }
      if (auto x = fo.find("sc"); x != fo.end() && x->second.isString())
        f.sidechainSource = x->second.string();
          if (!f.uri.empty())
            t.fx.push_back(std::move(f));
        }
      }

      if (auto v = to.find("notes"); v != to.end() && v->second.isArray()) {
        for (const Json& nj : v->second.array()) {
          if (!nj.isObject())
            continue;
          const auto& no = nj.object();
          NoteEvent n;
          if (auto x = no.find("channel"); x != no.end() && x->second.isNumber())
            n.channel = static_cast<uint8_t>(x->second.number());
          if (auto x = no.find("pitch"); x != no.end() && x->second.isNumber())
            n.pitch = static_cast<uint8_t>(x->second.number());
          if (auto x = no.find("velocity"); x != no.end() && x->second.isNumber())
            n.velocity = static_cast<uint8_t>(x->second.number());
          if (auto x = no.find("startBeats"); x != no.end() && x->second.isNumber())
            n.startBeats = x->second.number();
          if (auto x = no.find("durationBeats"); x != no.end() && x->second.isNumber())
            n.durationBeats = x->second.number();
          t.notes.push_back(n);
        }
      }

      if (auto v = to.find("regions"); v != to.end() && v->second.isArray()) {
        for (const Json& rj : v->second.array()) {
          if (!rj.isObject())
            continue;
          const auto& ro = rj.object();
          MidiRegion r;
          if (auto x = ro.find("id"); x != ro.end() && x->second.isString())
            r.id = x->second.string();
          else
            r.id = s.newRegionId();
          if (auto x = ro.find("name"); x != ro.end() && x->second.isString())
            r.name = x->second.string();
          if (auto x = ro.find("startBeats"); x != ro.end() && x->second.isNumber())
            r.startBeats = x->second.number();
          if (auto x = ro.find("durationBeats"); x != ro.end() && x->second.isNumber())
            r.durationBeats = x->second.number();
          if (auto x = ro.find("notes"); x != ro.end() && x->second.isArray()) {
            for (const Json& nj : x->second.array()) {
              if (!nj.isObject())
                continue;
              const auto& no = nj.object();
              NoteEvent n;
              if (auto y = no.find("channel"); y != no.end() && y->second.isNumber())
                n.channel = static_cast<uint8_t>(y->second.number());
              if (auto y = no.find("pitch"); y != no.end() && y->second.isNumber())
                n.pitch = static_cast<uint8_t>(y->second.number());
              if (auto y = no.find("velocity"); y != no.end() && y->second.isNumber())
                n.velocity = static_cast<uint8_t>(y->second.number());
              if (auto y = no.find("startBeats"); y != no.end() && y->second.isNumber())
                n.startBeats = y->second.number();
              if (auto y = no.find("durationBeats"); y != no.end() && y->second.isNumber())
                n.durationBeats = y->second.number();
              r.notes.push_back(n);
            }
          }
          t.regions.push_back(std::move(r));
        }
      }

      if (auto v = to.find("clips"); v != to.end() && v->second.isArray()) {
        for (const Json& cj : v->second.array()) {
          if (!cj.isObject())
            continue;
          const auto& co = cj.object();
          AudioClip c;
          if (auto x = co.find("id"); x != co.end() && x->second.isString())
            c.id = x->second.string();
          else
            c.id = s.newClipId();
          if (auto x = co.find("name"); x != co.end() && x->second.isString())
            c.name = x->second.string();
          if (auto x = co.find("sourceFile"); x != co.end() && x->second.isString())
            c.sourceFile = x->second.string();
          if (auto x = co.find("startBeats"); x != co.end() && x->second.isNumber())
            c.startBeats = x->second.number();
          if (auto x = co.find("durationBeats"); x != co.end() && x->second.isNumber())
            c.durationBeats = x->second.number();
          if (auto x = co.find("clipOffsetBeats"); x != co.end() && x->second.isNumber())
            c.clipOffsetBeats = x->second.number();
          if (auto x = co.find("gainDb"); x != co.end() && x->second.isNumber())
            c.gainDb = static_cast<float>(x->second.number());
          if (auto x = co.find("fadeInBeats"); x != co.end() && x->second.isNumber())
            c.fadeInBeats = x->second.number();
          if (auto x = co.find("fadeOutBeats"); x != co.end() && x->second.isNumber())
            c.fadeOutBeats = x->second.number();
          if (auto x = co.find("sampleRate"); x != co.end() && x->second.isNumber())
            c.sampleRate = static_cast<uint32_t>(x->second.number());
          if (auto x = co.find("channels"); x != co.end() && x->second.isNumber())
            c.channels = static_cast<uint16_t>(x->second.number());
          t.clips.push_back(std::move(c));
        }
      }

      if (auto x = to.find("recordArm"); x != to.end() && x->second.isBool())
        t.recordArm = x->second.boolean();

      if (auto v = to.find("takes"); v != to.end() && v->second.isArray()) {
        for (const Json& kj : v->second.array()) {
          if (!kj.isObject())
            continue;
          const auto& ko = kj.object();
          RecordTake k;
          if (auto x = ko.find("id"); x != ko.end() && x->second.isString())
            k.id = x->second.string();
          else
            k.id = s.newTakeId();
          if (auto x = ko.find("name"); x != ko.end() && x->second.isString())
            k.name = x->second.string();
          if (auto x = ko.find("sourceFile"); x != ko.end() && x->second.isString())
            k.sourceFile = x->second.string();
          if (auto x = ko.find("startBeats"); x != ko.end() && x->second.isNumber())
            k.startBeats = x->second.number();
          if (auto x = ko.find("durationBeats"); x != ko.end() && x->second.isNumber())
            k.durationBeats = x->second.number();
          if (auto x = ko.find("gainDb"); x != ko.end() && x->second.isNumber())
            k.gainDb = static_cast<float>(x->second.number());
          if (auto x = ko.find("sampleRate"); x != ko.end() && x->second.isNumber())
            k.sampleRate = static_cast<uint32_t>(x->second.number());
          if (auto x = ko.find("channels"); x != ko.end() && x->second.isNumber())
            k.channels = static_cast<uint16_t>(x->second.number());
          if (auto x = ko.find("clipId"); x != ko.end() && x->second.isString())
            k.clipId = x->second.string();
          t.takes.push_back(std::move(k));
        }
      }

      if (auto v = to.find("automation"); v != to.end() && v->second.isArray()) {
        for (const Json& aj : v->second.array()) {
          if (!aj.isObject())
            continue;
          const auto& ao = aj.object();
          AutomationLane l;
          if (auto x = ao.find("id"); x != ao.end() && x->second.isString())
            l.id = x->second.string();
          else
            l.id = s.newAutomationId();
          if (auto x = ao.find("param"); x != ao.end() && x->second.isString())
            l.param = x->second.string();
          if (auto x = ao.find("muted"); x != ao.end() && x->second.isBool())
            l.muted = x->second.boolean();
          if (auto x = ao.find("armed"); x != ao.end() && x->second.isBool())
            l.armed = x->second.boolean();
          if (auto x = ao.find("points"); x != ao.end() && x->second.isArray()) {
            for (const Json& pj : x->second.array()) {
              if (!pj.isObject())
                continue;
              const auto& po = pj.object();
              AutomationPoint p;
              if (auto y = po.find("timeBeats"); y != po.end() && y->second.isNumber())
                p.timeBeats = y->second.number();
              if (auto y = po.find("value"); y != po.end() && y->second.isNumber())
                p.value = y->second.number();
              l.points.push_back(p);
            }
            std::sort(l.points.begin(), l.points.end(),
                      [](const AutomationPoint& a, const AutomationPoint& b) {
                        return a.timeBeats < b.timeBeats;
                      });
          }
          t.automation.push_back(std::move(l));
        }
      }

      // Migration: legacy documents stored the whole track's notes as one
      // flat absolute list. Fold them into a single region (local
      // coordinates) so the rest of the app speaks one language. A track
      // that already carries at least one region has no legacy notes.
      if (t.regions.empty() && !t.notes.empty()) {
        double minStart = t.notes.front().startBeats;
        double maxEnd = t.notes.front().startBeats;
        for (const NoteEvent& n : t.notes) {
          minStart = std::min(minStart, n.startBeats);
          maxEnd = std::max(maxEnd, n.startBeats + n.durationBeats);
        }
        if (minStart < 0.0)
          minStart = 0.0;
        MidiRegion mig;
        mig.id = s.newRegionId();
        mig.name = "MIDI 1";
        mig.startBeats = minStart;
        mig.durationBeats = std::max(1.0, maxEnd - minStart);
        for (const NoteEvent& n : t.notes) {
          NoteEvent local = n;
          local.startBeats = n.startBeats - minStart;
          mig.notes.push_back(local);
        }
        t.regions.push_back(std::move(mig));
        t.notes.clear();
      }

      s.tracks_.push_back(std::move(t));
    }
  }

  if (auto f = root.find("buses"); f != root.end() && f->second.isArray()) {
    for (const Json& bj : f->second.array()) {
      if (!bj.isObject())
        continue;
      const auto& bo = bj.object();
      Bus b;
      if (auto v = bo.find("id"); v != bo.end() && v->second.isString())
        b.id = v->second.string();
      else
        b.id = s.newBusId();
      if (auto v = bo.find("name"); v != bo.end() && v->second.isString())
        b.name = v->second.string();
      if (auto v = bo.find("gainDb"); v != bo.end() && v->second.isNumber())
        b.faderDb = static_cast<float>(v->second.number());
      if (auto v = bo.find("pan"); v != bo.end() && v->second.isNumber())
        b.pan = static_cast<float>(v->second.number());
      if (auto v = bo.find("muted"); v != bo.end() && v->second.isBool())
        b.muted = v->second.boolean();
      if (auto v = bo.find("solo"); v != bo.end() && v->second.isBool())
        b.solo = v->second.boolean();
      if (auto v = bo.find("target"); v != bo.end() && v->second.isString())
        b.target = v->second.string();
      if (auto v = bo.find("fx"); v != bo.end() && v->second.isArray()) {
        for (const Json& fj : v->second.array()) {
          if (!fj.isObject())
            continue;
          const auto& fo = fj.object();
          TrackFx f;
          if (auto x = fo.find("id"); x != fo.end() && x->second.isString())
            f.id = x->second.string();
          else
            f.id = s.newFxId();
          if (auto x = fo.find("uri"); x != fo.end() && x->second.isString())
            f.uri = x->second.string();
          if (auto x = fo.find("enabled"); x != fo.end() && x->second.isBool())
            f.enabled = x->second.boolean();
          if (auto x = fo.find("sc"); x != fo.end() && x->second.isString())
            f.sidechainSource = x->second.string();
          if (auto x = fo.find("params");
              x != fo.end() && x->second.isArray()) {
            for (const Json& pj : x->second.array()) {
              if (!pj.isObject())
                continue;
              const auto& po = pj.object();
              FxParam p;
              if (auto y = po.find("index");
                  y != po.end() && y->second.isNumber())
                p.index = static_cast<uint32_t>(y->second.number());
              if (auto y = po.find("value");
                  y != po.end() && y->second.isNumber())
                p.value = static_cast<float>(y->second.number());
              f.params.push_back(p);
            }
          }
          if (!f.uri.empty())
            b.fx.push_back(std::move(f));
        }
      }
      s.buses_.push_back(std::move(b));
    }
  }

  // Parallel sends (Comp19).
  if (auto f = root.find("sends"); f != root.end() && f->second.isArray()) {
    for (const Json& sj : f->second.array()) {
      if (!sj.isObject())
        continue;
      const auto& so = sj.object();
      Send sn;
      if (auto v = so.find("id"); v != so.end() && v->second.isString())
        sn.id = v->second.string();
      else
        sn.id = s.newSendId();
      if (auto v = so.find("sourceTrack");
          v != so.end() && v->second.isString())
        sn.sourceTrack = v->second.string();
      if (auto v = so.find("sourceBus"); v != so.end() && v->second.isString())
        sn.sourceBus = v->second.string();
      if (auto v = so.find("targetBus");
          v != so.end() && v->second.isString())
        sn.targetBus = v->second.string();
      if (auto v = so.find("levelDb"); v != so.end() && v->second.isNumber())
        sn.levelDb = static_cast<float>(v->second.number());
      if (auto v = so.find("enabled"); v != so.end() && v->second.isBool())
        sn.enabled = v->second.boolean();
      // Keep only well-formed sends: a target bus plus exactly one source.
      const bool hasT = !sn.sourceTrack.empty();
      const bool hasB = !sn.sourceBus.empty();
      if (!sn.targetBus.empty() && hasT != hasB)
        s.sends_.push_back(std::move(sn));
    }
  }

  // Channel rack (step sequencer) ----------------------------------------
  if (auto f = root.find("channelRack"); f != root.end() && f->second.isObject()) {
    const auto& cro = f->second.object();
    if (auto v = cro.find("activePattern"); v != cro.end() && v->second.isNumber())
      s.channelRack_.activePattern = std::clamp(static_cast<int>(v->second.number()), 0, kPatternBanks - 1);
    if (auto v = cro.find("swing"); v != cro.end() && v->second.isNumber())
      s.channelRack_.swing = std::clamp(static_cast<int>(v->second.number()), 0, kMaxSwing);
    if (auto v = cro.find("stepsPerPattern"); v != cro.end() && v->second.isNumber())
      s.channelRack_.stepsPerPattern =
          std::clamp(static_cast<int>(v->second.number()), 4, kMaxStepCount);
    if (auto v = cro.find("channels"); v != cro.end() && v->second.isArray()) {
      // Base64 decode table.
      static constexpr int kDec[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
      };
      auto b64decode = [](const std::string& in) -> std::string {
        std::vector<unsigned char> tmp;
        tmp.reserve(in.size());
        for (char ch : in) {
          if (ch == '=' || ch == '\n' || ch == '\r')
            continue;
          int v = kDec[static_cast<unsigned char>(ch)];
          if (v >= 0)
            tmp.push_back(static_cast<unsigned char>(v));
        }
        std::string out;
        out.reserve((tmp.size() / 4) * 3 + 2);
        size_t i = 0;
        for (; i + 3 < tmp.size(); i += 4) {
          unsigned n = (static_cast<unsigned>(tmp[i]) << 18) |
                       (static_cast<unsigned>(tmp[i + 1]) << 12) |
                       (static_cast<unsigned>(tmp[i + 2]) << 6) |
                       static_cast<unsigned>(tmp[i + 3]);
          out += static_cast<char>((n >> 16) & 0xFF);
          out += static_cast<char>((n >> 8) & 0xFF);
          out += static_cast<char>(n & 0xFF);
        }
        // Trailing partial group: 2 sextets → 1 byte, 3 sextets → 2 bytes.
        if (i + 1 < tmp.size()) {
          unsigned n = (static_cast<unsigned>(tmp[i]) << 12) |
                       (static_cast<unsigned>(tmp[i + 1]) << 6);
          if (i + 2 < tmp.size())
            n |= static_cast<unsigned>(tmp[i + 2]);
          out += static_cast<char>((n >> 10) & 0xFF);
          if (i + 2 < tmp.size())
            out += static_cast<char>((n >> 2) & 0xFF);
        }
        return out;
      };
      for (const Json& chj : v->second.array()) {
        if (!chj.isObject())
          continue;
        const auto& cho = chj.object();
        StepChannel ch;
        // Legacy documents carry no "id"/"type"; allocate a fresh identity on
        // load so every channel still satisfies the id/name separation.
        if (auto x = cho.find("id"); x != cho.end() && x->second.isString() &&
                             !x->second.string().empty())
          ch.id = x->second.string();
        else
          ch.id = s.newChannelId();
        if (auto x = cho.find("name"); x != cho.end() && x->second.isString())
          ch.name = x->second.string();
        if (auto x = cho.find("type"); x != cho.end() && x->second.isString()) {
          const std::string t = x->second.string();
          if (t == "instrument") ch.type = ChannelType::Instrument;
          else if (t == "plugin") ch.type = ChannelType::Plugin;
          else if (t == "audioClip") ch.type = ChannelType::AudioClip;
          else if (t == "automation") ch.type = ChannelType::Automation;
          else if (t == "midi") ch.type = ChannelType::Midi;
        }
        if (auto x = cho.find("muted"); x != cho.end() && x->second.isBool())
          ch.muted = x->second.boolean();
        if (auto x = cho.find("solo"); x != cho.end() && x->second.isBool())
          ch.solo = x->second.boolean();
        if (auto x = cho.find("volume"); x != cho.end() && x->second.isNumber())
          ch.volume = static_cast<float>(x->second.number());
        if (auto x = cho.find("pan"); x != cho.end() && x->second.isNumber())
          ch.pan = static_cast<float>(x->second.number());
        if (auto x = cho.find("sound"); x != cho.end() && x->second.isString())
          ch.soundFile = x->second.string();
        // Decode step lanes: "steps0".."steps3" (base64-encoded byte arrays,
        // active length = the rack's stepsPerPattern).
        {
          const int sp =
              std::clamp(s.channelRack_.stepsPerPattern, 4, kMaxStepCount);
          for (int b = 0; b < kPatternBanks; ++b) {
            const std::string key = "steps" + std::to_string(b);
            if (auto x = cho.find(key); x != cho.end() && x->second.isString()) {
              std::string decoded = b64decode(x->second.string());
              for (int i = 0;
                   i < sp && i < static_cast<int>(decoded.size()); ++i) {
                // 0 = off. Legacy documents wrote '\1' for "on" (gate-only);
                // reinterpret that as the default velocity so old projects keep
                // their loudness. Real velocity bytes (>=2) load as-is.
                const uint8_t v = static_cast<uint8_t>(decoded[i]);
                ch.steps[b][i] = v == 0 ? 0 : v == 1 ? kStepDefaultVelocity : v;
              }
            }
          }
          // Legacy: single "st" key → bank 0.
          if (auto x = cho.find("st"); x != cho.end() && x->second.isString()) {
            std::string decoded = b64decode(x->second.string());
            for (int i = 0;
                 i < sp && i < static_cast<int>(decoded.size()); ++i) {
              const uint8_t v = static_cast<uint8_t>(decoded[i]);
              ch.steps[0][i] = v == 0 ? 0 : v == 1 ? kStepDefaultVelocity : v;
            }
          }
        }
        s.channelRack_.channels.push_back(std::move(ch));
      }
    }
  }

  // Keep the id counters ahead of any loaded tracks/clips so new ids never
  // collide.
  for (const Track& t : s.tracks_) {
    if (t.id.rfind("track-", 0) == 0) {
      const std::string num = t.id.substr(6);
      try {
        const uint32_t v = static_cast<uint32_t>(std::stoul(num));
        if (v > s.trackCounter_)
          s.trackCounter_ = v;
      } catch (const std::exception&) {
      }
    }
    for (const AudioClip& c : t.clips) {
      if (c.id.rfind("clip-", 0) == 0) {
        const std::string num = c.id.substr(5);
        try {
          const uint32_t v = static_cast<uint32_t>(std::stoul(num));
          if (v > s.clipCounter_)
            s.clipCounter_ = v;
        } catch (const std::exception&) {
        }
      }
    }
    for (const MidiRegion& r : t.regions) {
      if (r.id.rfind("region-", 0) == 0) {
        const std::string num = r.id.substr(7);
        try {
          const uint32_t v = static_cast<uint32_t>(std::stoul(num));
          if (v > s.regionCounter_)
            s.regionCounter_ = v;
        } catch (const std::exception&) {
        }
      }
    }
    for (const TrackFx& f : t.fx) {
      if (f.id.rfind("fx-", 0) == 0) {
        const std::string num = f.id.substr(3);
        try {
          const uint32_t v = static_cast<uint32_t>(std::stoul(num));
          if (v > s.fxCounter_)
            s.fxCounter_ = v;
        } catch (const std::exception&) {
        }
      }
    }
    for (const RecordTake& k : t.takes) {
      if (k.id.rfind("take-", 0) == 0) {
        const std::string num = k.id.substr(5);
        try {
          const uint32_t v = static_cast<uint32_t>(std::stoul(num));
          if (v > s.takeCounter_)
            s.takeCounter_ = v;
        } catch (const std::exception&) {
        }
      }
    }
  }
  for (const Bus& b : s.buses_) {
    if (b.id.rfind("bus-", 0) != 0)
      continue;
    const std::string num = b.id.substr(4);
    try {
      const uint32_t v = static_cast<uint32_t>(std::stoul(num));
      if (v > s.busCounter_)
        s.busCounter_ = v;
    } catch (const std::exception&) {
    }
  }

  return s;
}

namespace {

std::string normalizeBaseDir(const std::string& baseDir) {
  std::string base = baseDir;
  while (base.size() > 1 && base.back() == '/')
    base.pop_back();
  return base;
}

bool startsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() &&
         s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

void Session::relativizeAssets(const std::string& baseDir) {
  const std::string base = normalizeBaseDir(baseDir);
  if (base.empty())
    return;
  const std::string prefix = base + "/";
  auto relativize = [&prefix](std::string& src) {
    if (src.empty() || src.front() != '/')
      return; // already relative or an in-memory-only reference
    if (!startsWith(src, prefix))
      return; // external asset: keep absolute so it stays valid
    src = src.substr(prefix.size());
  };
  for (Track& t : tracks_) {
    for (AudioClip& c : t.clips)
      relativize(c.sourceFile);
    for (RecordTake& k : t.takes)
      relativize(k.sourceFile);
  }
  for (StepChannel& ch : channelRack_.channels)
    relativize(ch.soundFile);
}

void Session::resolveAssets(const std::string& baseDir) {
  const std::string base = normalizeBaseDir(baseDir);
  if (base.empty())
    return;
  for (Track& t : tracks_) {
    for (AudioClip& c : t.clips) {
      if (c.sourceFile.empty() || c.sourceFile.front() == '/')
        continue; // absolute (or empty) already resolves
      c.sourceFile = base + "/" + c.sourceFile;
    }
    for (RecordTake& k : t.takes) {
      if (k.sourceFile.empty() || k.sourceFile.front() == '/')
        continue;
      k.sourceFile = base + "/" + k.sourceFile;
    }
  }
  for (StepChannel& ch : channelRack_.channels) {
    if (ch.soundFile.empty() || ch.soundFile.front() == '/')
      continue;
    ch.soundFile = base + "/" + ch.soundFile;
  }
}

} // namespace misuc::project