#include "test_utils.h"

#include <cmath>

#include <misuc/engine/core/Json.h>
#include <misuc/engine/project/Session.h>

using misuc::core::Json;
using misuc::project::MidiRegion;
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
  s.setArtist("The Artist");
  s.setTempo(97.5);
  s.setTimeSignature(3, 4);
  s.setSampleRate(44100);

  const Session loaded = Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.name() == "My First Track");
  TEST_ASSERT(loaded.artist() == "The Artist");
  TEST_ASSERT(loaded.tempo() == 97.5);
  TEST_ASSERT(loaded.timeSigNumerator() == 3);
  TEST_ASSERT(loaded.timeSigDenominator() == 4);
  TEST_ASSERT(loaded.sampleRate() == 44100);
}

TEST_CASE(session_loop_roundtrip) {
  Session s;
  s.setLoop(true, 8.0, 16.0);
  const Session loaded = Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.loopMode());
  TEST_ASSERT(loaded.loopStart() == 8.0);
  TEST_ASSERT(loaded.loopEnd() == 16.0);
}

TEST_CASE(session_loop_disabled_overrides_range) {
  Session s;
  s.setLoop(true, 2.0, 8.0);
  s.setLoop(false, 2.0, 8.0); // disabling with equal bounds clears the flag
  TEST_ASSERT(!s.loopMode());
  TEST_ASSERT(Session::fromJson(s.toJson()).loopEnd() == 8.0);
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
  // Legacy whole-track notes are migrated into a single region at load: the
  // flat absolute list becomes a region at the span's start with local note
  // offsets, and the legacy list is emptied so playable state is unambiguous.
  TEST_ASSERT(loaded.tracks()[0].regions.size() == 1);
  TEST_ASSERT(loaded.tracks()[0].notes.empty());
  if (loaded.tracks()[0].regions.size() == 1) {
    const MidiRegion& mig = loaded.tracks()[0].regions[0];
    TEST_ASSERT(mig.startBeats == 0.0);
    TEST_ASSERT(mig.notes.size() == 2);
    TEST_ASSERT(mig.notes[0].pitch == 36);
    TEST_ASSERT(mig.notes[0].startBeats == 0.0);
    TEST_ASSERT(mig.notes[0].durationBeats == 2.0);
    TEST_ASSERT(mig.notes[1].pitch == 43);
    TEST_ASSERT(mig.notes[1].startBeats == 2.0);
    TEST_ASSERT(mig.notes[1].velocity == 90);
  }
}

TEST_CASE(session_regions_roundtrip) {
  Session s;
  const std::string tid = s.addTrack("Lead");
  const std::string rid =
      s.addRegion(tid, "Main Riff", 8.0, 4.0);
  TEST_ASSERT(!rid.empty());
  MidiRegion* r = s.findRegion(tid, rid);
  TEST_ASSERT(r != nullptr);
  TEST_ASSERT(r->name == "Main Riff");
  TEST_ASSERT(r->startBeats == 8.0);
  TEST_ASSERT(r->durationBeats == 4.0);
  r->notes.push_back(NoteEvent{0, 72, 100, 0.0, 0.5});
  r->notes.push_back(NoteEvent{0, 76, 90, 1.0, 1.0});

  const Session loaded = Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.tracks().size() == 1);
  TEST_ASSERT(loaded.tracks()[0].regions.size() == 1);
  if (loaded.tracks()[0].regions.empty())
    return;
  const MidiRegion& rr = loaded.tracks()[0].regions[0];
  TEST_ASSERT(rr.id == rid);
  TEST_ASSERT(rr.name == "Main Riff");
  TEST_ASSERT(rr.startBeats == 8.0);
  TEST_ASSERT(rr.durationBeats == 4.0);
  TEST_ASSERT(rr.notes.size() == 2);
  TEST_ASSERT(rr.notes[0].pitch == 72);
  TEST_ASSERT(rr.notes[0].startBeats == 0.0);
  TEST_ASSERT(rr.notes[1].startBeats == 1.0);
  // A track with regions keeps its (empty) legacy list; no migration runs.
  TEST_ASSERT(loaded.tracks()[0].notes.empty());
}

TEST_CASE(session_region_helpers) {
  Session s;
  const std::string tid = s.addTrack("Drums");
  const std::string rid = s.addRegion(tid, "Kit Loop", 4.0, 2.0);
  TEST_ASSERT(!rid.empty());
  MidiRegion* r = s.findRegion(tid, rid);
  TEST_ASSERT(r != nullptr);
  r->notes.push_back(NoteEvent{0, 36, 110, 0.0, 1.0});
  r->notes.push_back(NoteEvent{0, 38, 110, 0.5, 0.5});

  const std::string copyId = s.duplicateRegion(tid, rid);
  TEST_ASSERT(!copyId.empty());
  TEST_ASSERT(copyId != rid);
  TEST_ASSERT(s.findRegion(tid, copyId) != nullptr);
  const MidiRegion& copy = *s.findRegion(tid, copyId);
  TEST_ASSERT(copy.startBeats == 4.0 + 2.0); // placed right after the source
  TEST_ASSERT(copy.notes.size() == 2);
  TEST_ASSERT(copy.notes[0].startBeats == 0.0); // local coords preserved

  TEST_ASSERT(s.removeRegion(tid, copyId));
  TEST_ASSERT(s.findRegion(tid, copyId) == nullptr);
  TEST_ASSERT(s.findRegion(tid, rid) != nullptr);
  // Missing inputs fail gracefully.
  TEST_ASSERT(s.addRegion("nope", "x", 0, 1).empty());
  TEST_ASSERT(!s.removeRegion(tid, "nope"));
  TEST_ASSERT(s.duplicateRegion(tid, "nope").empty());
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

using misuc::project::AudioClip;

// Phase 12 Item #5: portable asset paths. MainWindow relativizes before
// writing (so a saved .misuc references its `<folder>/audio/` assets by
// relative path) and resolves after loading (so the running session holds
// absolute paths the engine can play).
TEST_CASE(session_clip_paths_roundtrip) {
  Session s;
  const std::string id = s.addTrack("Vocals");
  s.addClip(id, AudioClip{/*id*/ "c1", /*name*/ "Take 1",
                          /*sourceFile*/ "/home/me/proj/audio/take1.wav",
                          /*startBeats*/ 1.0, /*durationBeats*/ 2.0,
                          /*clipOffsetBeats*/ 0.0, /*gainDb*/ 0.0f,
                          /*fadeInBeats*/ 0.0, /*fadeOutBeats*/ 0.0,
                          /*sampleRate*/ 48000, /*channels*/ 1});

  // Save-side: absolute path under the project folder becomes a relative one.
  Session copy = s;
  copy.relativizeAssets("/home/me/proj");
  const std::string& stored = copy.tracks()[0].clips[0].sourceFile;
  TEST_ASSERT(stored == "audio/take1.wav");

  // The live session that gets serialized is unmodified.
  TEST_ASSERT(s.tracks()[0].clips[0].sourceFile ==
              "/home/me/proj/audio/take1.wav");

  // Load-side: relative path resolves back against the project folder.
  Session saved = Session::fromJson(copy.toJson());
  saved.resolveAssets("/home/me/proj");
  TEST_ASSERT(saved.tracks()[0].clips[0].sourceFile ==
              "/home/me/proj/audio/take1.wav");
}

TEST_CASE(session_clip_paths_external_assets_stay_absolute) {
  Session s;
  const std::string id = s.addTrack("Loop");
  s.addClip(id, AudioClip{/*id*/ "c1", /*name*/ "Sample",
                          /*sourceFile*/ "/opt/samples/beat.wav",
                          /*startBeats*/ 0.0, /*durationBeats*/ 4.0,
                          /*clipOffsetBeats*/ 0.0, /*gainDb*/ 0.0f,
                          /*fadeInBeats*/ 0.0, /*fadeOutBeats*/ 0.0,
                          /*sampleRate*/ 44100, /*channels*/ 2});

  // A file outside the project folder must keep its absolute reference.
  Session copy = s;
  copy.relativizeAssets("/home/me/proj");
  TEST_ASSERT(copy.tracks()[0].clips[0].sourceFile ==
              "/opt/samples/beat.wav");

  // Absolute (and `~`-style) references survive a resolve unchanged.
  Session loaded = Session::fromJson(copy.toJson());
  loaded.resolveAssets("/home/me/proj");
  TEST_ASSERT(loaded.tracks()[0].clips[0].sourceFile ==
              "/opt/samples/beat.wav");

  Session rel;
  const std::string rid = rel.addTrack("Rel");
  rel.addClip(rid, AudioClip{/*id*/ "c1", /*name*/ "R",
                             /*sourceFile*/ "audio/in.wav", /*startBeats*/ 0.0,
                             /*durationBeats*/ 1.0, /*clipOffsetBeats*/ 0.0,
                             /*gainDb*/ 0.0f, /*fadeInBeats*/ 0.0,
                             /*fadeOutBeats*/ 0.0, /*sampleRate*/ 48000,
                             /*channels*/ 2});
  rel.resolveAssets("/home/me/proj");
  TEST_ASSERT(rel.tracks()[0].clips[0].sourceFile ==
              "/home/me/proj/audio/in.wav");
}

TEST_CASE(session_rack_sound_files_track_clip_assets) {
  // Sampler-channel sound references (Browser → Channel Rack drops) are
  // portable project assets exactly like clip sources: relativized on save,
  // resolved on load, external paths left absolute.
  Session s;
  auto& cr = s.channelRack();
  misuc::project::StepChannel kick;
  kick.id = s.newChannelId();
  kick.name = "Kick";
  kick.soundFile = "/home/me/proj/samples/kick_01.wav";
  cr.channels.push_back(kick);

  Session save = s;
  save.relativizeAssets("/home/me/proj");
  TEST_ASSERT(save.channelRack().channels[0].soundFile ==
              "samples/kick_01.wav");

  Session loaded = Session::fromJson(save.toJson());
  TEST_ASSERT(loaded.channelRack().channels[0].soundFile ==
              "samples/kick_01.wav");
  loaded.resolveAssets("/home/me/proj");
  TEST_ASSERT(loaded.channelRack().channels[0].soundFile ==
              "/home/me/proj/samples/kick_01.wav");

  // A sound outside the project folder keeps its absolute reference.
  Session ext = s;
  ext.channelRack().channels[0].soundFile = "/opt/samples/hit.wav";
  ext.relativizeAssets("/home/me/proj");
  TEST_ASSERT(ext.channelRack().channels[0].soundFile ==
              "/opt/samples/hit.wav");
}