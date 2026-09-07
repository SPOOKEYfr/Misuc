#include "test_utils.h"

#include <cmath>

#include <misuc/engine/core/Json.h>
#include <misuc/engine/project/Session.h>

using misuc::core::Json;
using misuc::project::NoteEvent;
using misuc::project::Session;
using misuc::project::Track;

TEST_CASE(json_roundtrip) {
  Json::Object o;
  o.emplace("name", Json("test project"));
  o.emplace("tempo", Json(128.5));
  o.emplace("loop", Json(true));
  o.emplace("null", Json(nullptr));

  Json::Array arr;
  arr.emplace_back(Json("a"));
  arr.emplace_back(Json(2.0));
  o.emplace("tags", Json(std::move(arr)));

  const std::string text = Json(std::move(o)).dump();
  const Json parsed = Json::parse(text);
  TEST_ASSERT(parsed.isObject());
  TEST_ASSERT(parsed.at("name").string() == "test project");
  TEST_ASSERT(parsed.at("tempo").number() == 128.5);
  TEST_ASSERT(parsed.at("loop").boolean());
  TEST_ASSERT(parsed.at("null").isNull());
  TEST_ASSERT(parsed.at("tags").array().size() == 2);
  TEST_ASSERT(parsed.at("tags").array()[1].number() == 2.0);
}

TEST_CASE(json_parse_errors) {
  bool threw = false;
  try {
    Json::parse("{ invalid");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  TEST_ASSERT(threw);
}

TEST_CASE(session_roundtrip) {
  Session s;
  s.setName("My First Track");
  s.setTempo(97.5);
  s.setTimeSignature(3, 4);
  s.setSampleRate(44100);

  const Session loaded = Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.name() == "My First Track");
  TEST_ASSERT(loaded.tempo() == 97.5);
  TEST_ASSERT(loaded.timeSigNumerator() == 3);
  TEST_ASSERT(loaded.timeSigDenominator() == 4);
  TEST_ASSERT(loaded.sampleRate() == 44100);
}

TEST_CASE(session_schema_version) {
  const Session s;
  const Json doc = Json::parse(s.toJson());
  TEST_ASSERT(doc.at("schemaVersion").number() == Json::kSchemaVersion);
  TEST_ASSERT(doc.at("app").string() == "misuc");
}

TEST_CASE(session_tracks_roundtrip) {
  Session s;
  const std::string id = s.addTrack("Bass");
  Track* t = s.findTrack(id);
  TEST_ASSERT(t != nullptr);
  TEST_ASSERT(t->name == "Bass");
  t->notes.push_back(NoteEvent{0, 36, 110, 0.0, 2.0});
  t->notes.push_back(NoteEvent{0, 43, 90, 2.0, 1.5});
  t->muted = false;
  t->gainDb = -3.0f;

  const Session loaded = Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.tracks().size() == 1);
  TEST_ASSERT(loaded.tracks()[0].id == id);
  TEST_ASSERT(loaded.tracks()[0].name == "Bass");
  TEST_ASSERT(loaded.tracks()[0].muted == false);
  TEST_ASSERT(std::fabs(loaded.tracks()[0].gainDb - (-3.0f)) < 0.001f);
  TEST_ASSERT(loaded.tracks()[0].notes.size() == 2);
  TEST_ASSERT(loaded.tracks()[0].notes[0].pitch == 36);
  TEST_ASSERT(loaded.tracks()[0].notes[0].startBeats == 0.0);
  TEST_ASSERT(loaded.tracks()[0].notes[0].durationBeats == 2.0);
  TEST_ASSERT(loaded.tracks()[0].notes[1].velocity == 90);
  TEST_ASSERT(loaded.tracks()[0].notes[1].startBeats == 2.0);
}

TEST_CASE(session_track_lifecycle) {
  Session s;
  const std::string a = s.addTrack("Drums");
  const std::string b = s.addTrack("Keys");
  TEST_ASSERT(a != b);
  TEST_ASSERT(s.tracks().size() == 2);
  TEST_ASSERT(s.findTrack(a) != nullptr);
  TEST_ASSERT(s.removeTrack(a));
  TEST_ASSERT(s.tracks().size() == 1);
  TEST_ASSERT(s.findTrack(a) == nullptr);

  // Round-tripping must not collide new ids with loaded ones.
  const Session saved = Session::fromJson(s.toJson());
  Session copy = saved;
  const std::string c = copy.addTrack("NewOne");
  TEST_ASSERT(copy.findTrack(c) != nullptr);
  TEST_ASSERT(c != b); // fresh id, distinct from the surviving track
  TEST_ASSERT(copy.tracks().size() == 2);
}

TEST_CASE(session_reads_v1_document) {
  // v1 had no "tracks" key; loading must not throw and must stay empty.
  const std::string v1 =
      R"({"schemaVersion":1,"app":"misuc","metadata":{"name":"Old"},"transport":{"tempo":90,"timeSigNumerator":3,"timeSigDenominator":4,"sampleRate":44100}})";
  const Session loaded = Session::fromJson(v1);
  TEST_ASSERT(loaded.name() == "Old");
  TEST_ASSERT(loaded.tempo() == 90.0);
  TEST_ASSERT(loaded.timeSigNumerator() == 3);
  TEST_ASSERT(loaded.timeSigDenominator() == 4);
  TEST_ASSERT(loaded.tracks().empty());
}