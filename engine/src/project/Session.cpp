#include <misuc/engine/project/Session.h>

#include <algorithm>
#include <stdexcept>

namespace misuc::project {

using core::Json;

Session::Session() = default;

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

std::string Session::addTrack(std::string name) {
  Track t;
  t.id = newTrackId();
  t.name = std::move(name);
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
  root.emplace("schemaVersion", Json(Json::kSchemaVersion));
  root.emplace("app", Json("misuc"));

  Json::Object meta;
  meta.emplace("name", Json(name_));
  meta.emplace("createdUtc", Json(std::string("")));
  meta.emplace("modifiedUtc", Json(std::string("")));
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
  root.emplace("transport", Json(std::move(transportObj)));

  Json::Array tracks;
  for (const Track& t : tracks_) {
    Json::Object tobj;
    tobj.emplace("id", Json(t.id));
    tobj.emplace("name", Json(t.name));
    tobj.emplace("muted", Json(t.muted));
    tobj.emplace("solo", Json(t.solo));
    tobj.emplace("gainDb", Json(static_cast<double>(t.gainDb)));
    tobj.emplace("pan", Json(static_cast<double>(t.pan)));

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
    tracks.emplace_back(Json(std::move(tobj)));
  }
  root.emplace("tracks", Json(std::move(tracks)));

  return Json(std::move(root)).dump();
}

Session Session::fromJson(const std::string& text) {
  const Json doc = Json::parse(text);
  if (!doc.isObject())
    throw std::runtime_error("Project root must be a JSON object");

  Session s;
  const auto& root = doc.object();

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
  }

  if (auto f = root.find("metadata"); f != root.end() && f->second.isObject()) {
    const auto& m = f->second.object();
    if (auto n = m.find("name"); n != m.end() && n->second.isString())
      s.setName(n->second.string());
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
      if (auto v = to.find("muted"); v != to.end() && v->second.isBool())
        t.muted = v->second.boolean();
      if (auto v = to.find("solo"); v != to.end() && v->second.isBool())
        t.solo = v->second.boolean();
      if (auto v = to.find("gainDb"); v != to.end() && v->second.isNumber())
        t.gainDb = static_cast<float>(v->second.number());
      if (auto v = to.find("pan"); v != to.end() && v->second.isNumber())
        t.pan = static_cast<float>(v->second.number());

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
  for (Track& t : tracks_) {
    for (AudioClip& c : t.clips) {
      if (c.sourceFile.empty() || c.sourceFile.front() != '/')
        continue; // already relative or an in-memory-only reference
      if (!startsWith(c.sourceFile, prefix))
        continue; // external asset: keep absolute so it stays valid
      c.sourceFile = c.sourceFile.substr(prefix.size());
    }
  }
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
  }
}

} // namespace misuc::project