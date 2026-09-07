#include <misuc/engine/project/Session.h>

#include <stdexcept>

namespace misuc::project {

using core::Json;

Session::Session() = default;

std::string Session::newTrackId() {
  return "track-" + std::to_string(++trackCounter_);
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
  root.emplace("transport", Json(std::move(transportObj)));

  Json::Array tracks;
  for (const Track& t : tracks_) {
    Json::Object tobj;
    tobj.emplace("id", Json(t.id));
    tobj.emplace("name", Json(t.name));
    tobj.emplace("muted", Json(t.muted));
    tobj.emplace("gainDb", Json(static_cast<double>(t.gainDb)));

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
      if (auto v = to.find("gainDb"); v != to.end() && v->second.isNumber())
        t.gainDb = static_cast<float>(v->second.number());

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
      s.tracks_.push_back(std::move(t));
    }
  }

  // Keep the id counter ahead of any loaded track so new ids never collide.
  for (const Track& t : s.tracks_)
    if (t.id.rfind("track-", 0) == 0) {
      const std::string num = t.id.substr(6);
      try {
        const uint32_t v = static_cast<uint32_t>(std::stoul(num));
        if (v > s.trackCounter_)
          s.trackCounter_ = v;
      } catch (const std::exception&) {
      }
    }

  return s;
}

} // namespace misuc::project