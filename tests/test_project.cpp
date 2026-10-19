#include "test_utils.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <misuc/engine/core/Json.h>
#include <misuc/engine/core/ProjectFile.h>
#include <misuc/engine/fx/BuiltinFx.h>
#include <misuc/engine/project/Session.h>

using misuc::core::Json;
using misuc::core::readFile;
using misuc::core::atomicWrite;
using misuc::project::Session;

namespace {

std::filesystem::path tempDir() {
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "misuc_test_project";
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

TEST_CASE(projectfile_atomic_write) {
  const std::string path = (tempDir() / "atomic.misuc").string();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + ".tmp", ec);

  TEST_ASSERT(atomicWrite(path, R"({"x":1})"));
  const auto read = readFile(path);
  TEST_ASSERT(read.ok);
  TEST_ASSERT(read.content == R"({"x":1})");
  // No temp file is left behind after a successful atomic write.
  TEST_ASSERT(!std::filesystem::exists(path + ".tmp"));

  // Overwriting replaces content and still leaves no temp file.
  TEST_ASSERT(atomicWrite(path, R"({"x":2})"));
  TEST_ASSERT(readFile(path).content == R"({"x":2})");
  TEST_ASSERT(!std::filesystem::exists(path + ".tmp"));
}

TEST_CASE(projectfile_atomic_write_missing_dir_fails) {
  std::string err;
  const bool ok = atomicWrite("/nonexistent/misuc/dir/x.misuc", "{}", &err);
  TEST_ASSERT(!ok);
  TEST_ASSERT(!err.empty());
}

TEST_CASE(projectfile_read_missing) {
  const auto r =
      readFile((tempDir() / "definitely_not_here.misuc").string());
  TEST_ASSERT(!r.ok);
  TEST_ASSERT(r.missing);
}

TEST_CASE(projectfile_utc_timestamp_shape) {
  const std::string t = misuc::core::utcNowIso();
  // RFC-3339 UTC: YYYY-MM-DDTHH:MM:SSZ
  TEST_ASSERT(t.size() == 20);
  TEST_ASSERT(t[4] == '-' && t[7] == '-' && t[10] == 'T' && t[13] == ':' &&
              t[16] == ':' && t[19] == 'Z');
}

TEST_CASE(container_write_read_roundtrip) {
  using misuc::core::ContainerEntry;
  using misuc::core::readContainer;
  using misuc::core::writeContainer;
  const std::string path = (tempDir() / "roundtrip.misuc").string();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + ".tmp", ec);

  const std::string song = R"({"format":"misuc.song","schemaVersion":3,"track":1})";
  const std::vector<ContainerEntry> entries = {
      {misuc::core::kContainerSongEntry, song},
  };
  std::string err;
  TEST_ASSERT(writeContainer(path, entries, &err));
  TEST_ASSERT(err.empty());
  // No temp file left behind.
  TEST_ASSERT(!std::filesystem::exists(path + ".tmp"));

  const auto r = readContainer(path);
  TEST_ASSERT(r.ok);
  TEST_ASSERT(!r.legacyJson);
  TEST_ASSERT(r.error.empty());
  TEST_ASSERT(r.entries.size() == 1);
  if (r.entries.size() == 1) {
    TEST_ASSERT(r.entries[0].name == misuc::core::kContainerSongEntry);
    TEST_ASSERT(r.entries[0].data == song);
  }
}

TEST_CASE(container_roundtrip_multi_entry_assets) {
  using misuc::core::ContainerEntry;
  using misuc::core::readContainer;
  using misuc::core::writeContainer;
  const std::string path = (tempDir() / "multi.misuc").string();
  std::error_code ec;
  std::filesystem::remove(path, ec);

  // A larger song payload forces the deflate path (>32 bytes) plus a bundled
  // binary asset, so both compression methods are exercised.
  std::string song = R"({"format":"misuc.song","schemaVersion":3,"meta":")";
  std::string pad;
  for (int i = 0; i < 2000; ++i)
    pad += std::to_string(i) + ",";
  song = "{\"format\":\"misuc.song\",\"schemaVersion\":3,\"pad\":[" + pad + "]}";
  const std::string audio("RIFF\x00\x00\x00\x00WAVE\x00", 14);

  std::string err;
  TEST_ASSERT(writeContainer(
      path, {{misuc::core::kContainerSongEntry, song},
             {"assets/Audio/demo_pad.wav", audio}},
      &err));
  TEST_ASSERT(err.empty());

  const auto r = readContainer(path);
  TEST_ASSERT(r.ok);
  TEST_ASSERT(!r.legacyJson);
  TEST_ASSERT(r.entries.size() == 2);
  if (r.entries.size() == 2) {
    int songIdx = -1, wavIdx = -1;
    for (size_t i = 0; i < r.entries.size(); ++i) {
      if (r.entries[i].name == misuc::core::kContainerSongEntry)
        songIdx = static_cast<int>(i);
      if (r.entries[i].name == "assets/Audio/demo_pad.wav")
        wavIdx = static_cast<int>(i);
    }
    TEST_ASSERT(songIdx >= 0 && wavIdx >= 0);
    TEST_ASSERT(r.entries[songIdx].data == song);
    TEST_ASSERT(r.entries[wavIdx].data == audio);
  }
}

TEST_CASE(container_legacy_json_migrates) {
  using misuc::core::readContainer;
  const std::string path = (tempDir() / "legacy.misuc").string();
  std::error_code ec;
  std::filesystem::remove(path, ec);

  const std::string legacy =
      R"({"format":"misuc.song","schemaVersion":3,"metadata":{"name":"old"}})";
  TEST_ASSERT(misuc::core::atomicWrite(path, legacy, nullptr));
  const auto r = readContainer(path);
  TEST_ASSERT(r.ok);
  TEST_ASSERT(r.legacyJson);
  TEST_ASSERT(r.legacyText == legacy);
  TEST_ASSERT(r.entries.empty());
}

TEST_CASE(container_corruption_detected) {
  using misuc::core::readContainer;
  using misuc::core::writeContainer;
  const std::string path = (tempDir() / "corrupt.misuc").string();
  std::error_code ec;
  std::filesystem::remove(path, ec);

  const std::string song(600, 'x'); // deflates (compressible), then CRC-checked
  std::string err;
  TEST_ASSERT(writeContainer(path, {{misuc::core::kContainerSongEntry, song}}, &err));
  TEST_ASSERT(err.empty());

  // Flip a byte inside the compressed payload -> CRC mismatch must fail.
  {
    std::ifstream in(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    in.close();
    // Parse the local header (30B) + name length to find the data offset, then
    // corrupt the first payload byte (not the central directory).
    const size_t nameLen = size_t(uint8_t(data[26])) | (size_t(uint8_t(data[27])) << 8);
    const size_t extraLen = size_t(uint8_t(data[28])) | (size_t(uint8_t(data[29])) << 8);
    const size_t dataOff = 30 + nameLen + extraLen;
    TEST_ASSERT(dataOff + 1 < data.size());
    data[dataOff] ^= 0x55;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    const auto r = readContainer(path);
    TEST_ASSERT(!r.ok);
    TEST_ASSERT(!r.error.empty());
    std::filesystem::remove(path, ec);
  }

  // Truncate the tail (clip before the end-of-central-directory) -> fail.
  {
    TEST_ASSERT(writeContainer(path, {{misuc::core::kContainerSongEntry, song}}, nullptr));
    std::ifstream in(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    in.close();
    data.resize(data.size() - 16);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    const auto r = readContainer(path);
    TEST_ASSERT(!r.ok);
    TEST_ASSERT(!r.error.empty());
  }
}

TEST_CASE(container_missing_file) {
  using misuc::core::readContainer;
  const auto r = readContainer((tempDir() / "not_here.misuc").string());
  TEST_ASSERT(!r.ok);
  TEST_ASSERT(r.missing);
}

TEST_CASE(session_v3_headers) {
  const Session s;
  const Json doc = Json::parse(s.toJson());
  TEST_ASSERT(doc.at("format").string() == "misuc.song");
  TEST_ASSERT(doc.at("schemaVersion").number() == Json::kSchemaVersion);
  TEST_ASSERT(doc.at("app").string() == "misuc");
  // Metadata timestamps serialize (empty for a never-touched new session).
  TEST_ASSERT(doc.at("metadata").object().at("createdUtc").isString());
  TEST_ASSERT(doc.at("metadata").object().at("modifiedUtc").isString());
}

TEST_CASE(session_timestamp_stamps) {
  Session s;
  TEST_ASSERT(s.ensureCreatedUtc()); // first call stamps
  const std::string created = s.createdUtc();
  TEST_ASSERT(!created.empty());
  TEST_ASSERT(!s.ensureCreatedUtc()); // second call is a no-op
  TEST_ASSERT(s.createdUtc() == created);

  s.touchModified();
  TEST_ASSERT(!s.modifiedUtc().empty());

  // Stamps survive a round trip.
  const Session loaded = Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.createdUtc() == created);
  TEST_ASSERT(loaded.modifiedUtc() == s.modifiedUtc());
}

TEST_CASE(session_v3_roundtrip_is_deterministic) {
  Session s;
  s.setName("Determinism");
  s.setTempo(96.0);
  s.setCreatedUtc("2026-09-10T12:00:00Z");
  s.setModifiedUtc("2026-09-10T12:00:05Z");
  const std::string tid = s.addTrack("Bass");
  s.addRegion(tid, "Riff", 0.0, 4.0);

  const std::string text = s.toJson();
  const Session loaded = Session::fromJson(text);
  // Save -> load -> save must reproduce byte-identical output (timestamps are
  // stored fields, never regenerated inside toJson).
  TEST_ASSERT(loaded.toJson() == text);
  TEST_ASSERT(loaded.name() == "Determinism");
  TEST_ASSERT(loaded.tracks().size() == 1);
}

TEST_CASE(session_metronome_v4_roundtrip) {
  Session s;
  s.setMetronomeOn(true);
  s.setMetronomeVolume(0.35f);
  s.setCountInBeats(4.0f);

  const std::string text = s.toJson();
  const Session loaded = Session::fromJson(text);
  TEST_ASSERT(loaded.metronomeOn());
  TEST_ASSERT(std::fabs(loaded.metronomeVolume() - 0.35f) < 1e-6f);
  TEST_ASSERT(std::fabs(loaded.countInBeats() - 4.0f) < 1e-6f);
  // Deterministic save/load/save.
  TEST_ASSERT(loaded.toJson() == text);
}

TEST_CASE(session_metronome_defaults_for_legacy) {
  // A pre-v4 document has no metronome fields: it must load click-free with
  // the room-level default volume and 2-beat lead.
  const std::string old =
      R"({"format":"misuc.song","schemaVersion":3,"app":"misuc",)"
      R"("metadata":{"name":"Legacy"},"transport":{"tempo":120}})";
  const Session loaded = Session::fromJson(old);
  TEST_ASSERT(!loaded.metronomeOn());
  TEST_ASSERT(std::fabs(loaded.metronomeVolume() - 0.9f) < 1e-6f);
  TEST_ASSERT(std::fabs(loaded.countInBeats() - 2.0f) < 1e-6f);
}

TEST_CASE(session_rejects_future_schema_version) {
  const std::string future =
      R"({"format":"misuc.song","schemaVersion":999,"app":"misuc","metadata":{"name":"F"},"transport":{},"tracks":[],"buses":[]})";
  bool threw = false;
  try {
    Session::fromJson(future);
  } catch (const std::runtime_error& e) {
    threw = true;
    const std::string what = e.what();
    TEST_ASSERT(what.find("version") != std::string::npos);
    TEST_ASSERT(what.find("999") != std::string::npos);
  }
  TEST_ASSERT(threw);
}

TEST_CASE(session_rejects_foreign_format) {
  const std::string foreign =
      R"({"format":"flp.structural","schemaVersion":3,"app":"misuc"})";
  bool threw = false;
  try {
    Session::fromJson(foreign);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  TEST_ASSERT(threw);
}

TEST_CASE(session_accepts_legacy_no_format_marker) {
  // A v1 document predating the format marker must still load.
  const std::string v1 =
      R"({"schemaVersion":1,"app":"misuc","metadata":{"name":"Old"},"transport":{"tempo":90}})";
  const Session loaded = Session::fromJson(v1);
  TEST_ASSERT(loaded.name() == "Old");
  TEST_ASSERT(loaded.tempo() == 90.0);
  TEST_ASSERT(loaded.createdUtc().empty());
}

TEST_CASE(session_v2_migration_still_loads) {
  // v2 stored a schema version but no format marker; the loader must migrate
  // legacy whole-track notes into a region exactly like pre-v3 builds.
  const std::string v2 =
      R"({"schemaVersion":2,"app":"misuc","metadata":{"name":"Rev"},"transport":{"tempo":120},"tracks":[{"id":"track-1","name":"Bass","notes":[{"channel":0,"pitch":36,"velocity":100,"startBeats":0,"durationBeats":2},{"channel":0,"pitch":38,"velocity":90,"startBeats":2,"durationBeats":1}]}],"buses":[]})";
  const Session loaded = Session::fromJson(v2);
  TEST_ASSERT(loaded.name() == "Rev");
  TEST_ASSERT(loaded.tracks().size() == 1);
  TEST_ASSERT(loaded.tracks()[0].regions.size() == 1);
  TEST_ASSERT(loaded.tracks()[0].notes.empty());
  TEST_ASSERT(loaded.tracks()[0].regions[0].notes.size() == 2);
  TEST_ASSERT(loaded.tracks()[0].regions[0].notes[1].pitch == 38);
}

TEST_CASE(session_fromjson_rejects_non_object) {
  bool threw = false;
  try {
    Session::fromJson("\"just a string\"");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  TEST_ASSERT(threw);
}

TEST_CASE(session_channel_rack_roundtrip) {
  Session s;
  s.setName("Rack Test");
  auto& cr = s.channelRack();
  cr.activePattern = 2;
  cr.swing = 33;
  misuc::project::StepChannel kick;
  kick.name = "Kick";
  kick.volume = 0.9f;
  kick.pan = -0.3f;
  kick.soundFile = "/path/to/samples/kick_01.wav"; // browser-drop reference
  kick.steps[0][0] = 100;  // on, default velocity
  kick.steps[0][4] = 90;   // on, quieter
  kick.steps[0][8] = 100;
  kick.steps[0][12] = 127;
  kick.steps[2][2] = 64;
  cr.channels.push_back(kick);

  misuc::project::StepChannel snare;
  snare.name = "Snare";
  snare.muted = true;
  snare.steps[0][4] = 100;
  snare.steps[0][12] = 100;
  cr.channels.push_back(snare);

  const std::string json = s.toJson();
  const Session loaded = Session::fromJson(json);
  TEST_ASSERT(loaded.name() == "Rack Test");
  TEST_ASSERT(loaded.channelRack().channels.size() == 2);
  TEST_ASSERT(loaded.channelRack().activePattern == 2);
  TEST_ASSERT(loaded.channelRack().swing == 33);

  const auto& lk = loaded.channelRack().channels[0];
  TEST_ASSERT(lk.name == "Kick");
  TEST_ASSERT(lk.volume == 0.9f);
  TEST_ASSERT(lk.pan == -0.3f);
  TEST_ASSERT(lk.soundFile == "/path/to/samples/kick_01.wav");
  TEST_ASSERT(lk.steps[0][0] == 100);
  TEST_ASSERT(lk.steps[0][4] == 90);
  TEST_ASSERT(lk.steps[0][8] == 100);
  TEST_ASSERT(lk.steps[0][12] == 127);
  TEST_ASSERT(lk.steps[1][0] == 0);
  TEST_ASSERT(lk.steps[2][2] == 64);

  const auto& ls = loaded.channelRack().channels[1];
  TEST_ASSERT(ls.name == "Snare");
  TEST_ASSERT(ls.muted == true);
  TEST_ASSERT(ls.steps[0][4] == 100);
  TEST_ASSERT(ls.steps[0][12] == 100);
}

TEST_CASE(session_rack_pattern_materialization) {
  Session s;
  auto& cr = s.channelRack();
  cr.activePattern = 1;
  misuc::project::StepChannel kick;
  kick.name = "Kick";
  kick.steps[1][0] = 100;  // active bank 1
  kick.steps[1][8] = 40;   // quieter second hit
  kick.steps[0][0] = 100;  // other bank: must not materialize
  cr.channels.push_back(kick);
  misuc::project::StepChannel muted;
  muted.name = "Muted Rex";
  muted.muted = true;
  muted.steps[1][4] = 100; // muted channel: must not materialize
  cr.channels.push_back(muted);

  const std::string tid = s.addTrack("Sequencer");

  // Rack pitch mapping is exactly the seeded kit {A1,A3,E5,A1,A4} then A3.
  TEST_ASSERT(misuc::project::rackRowPitch(0) == 33);  // A1 (55 Hz)
  TEST_ASSERT(misuc::project::rackRowPitch(1) == 57);  // A3 (220 Hz)
  TEST_ASSERT(misuc::project::rackRowPitch(2) == 76);  // E5 (660 Hz)
  TEST_ASSERT(misuc::project::rackRowPitch(3) == 33);  // A1 (55 Hz)
  TEST_ASSERT(misuc::project::rackRowPitch(4) == 69);  // A4 (440 Hz)
  TEST_ASSERT(misuc::project::rackRowPitch(9) == 57);  // fallback A3

  // Materialize the active bank as a named region.
  const std::string rid =
      misuc::project::materializePatternRegion(s, tid, -1, 16.0, 4.0);
  TEST_ASSERT(!rid.empty());
  const auto* r = s.findRegion(tid, rid);
  TEST_ASSERT(r != nullptr);
  TEST_ASSERT(r->name == "Pattern 2");
  TEST_ASSERT(r->startBeats == 16.0);
  TEST_ASSERT(r->durationBeats == 4.0);
  // Only the unmuted Kick channel's bank-1 gates land (steps 0 and 8).
  TEST_ASSERT(r->notes.size() == 2);
  int sawVel100 = 0, sawVel40 = 0;
  for (const auto& n : r->notes) {
    TEST_ASSERT(n.pitch == 33); // kick row 0
    TEST_ASSERT(n.startBeats == 0.0 || n.startBeats == 2.0);
    TEST_ASSERT(n.durationBeats == 0.24);
    // Per-step velocities survive into the materialized notes.
    if (n.velocity == 100) sawVel100++;
    if (n.velocity == 40) sawVel40++;
  }
  TEST_ASSERT(sawVel100 == 1);
  TEST_ASSERT(sawVel40 == 1);

  // An explicit bank materializes that bank's gates, regardless of the rack's
  // active pattern.
  const std::string rid0 =
      misuc::project::materializePatternRegion(s, tid, 0, 0.0, 4.0);
  const auto* r0 = s.findRegion(tid, rid0);
  TEST_ASSERT(r0 != nullptr);
  TEST_ASSERT(r0->name == "Pattern 1");
  TEST_ASSERT(r0->notes.size() == 1);

  // Solo semantics: soloing one channel silences the rest of the kit.
  s.channelRack().channels[1].solo = true;
  const std::string ridS =
      misuc::project::materializePatternRegion(s, tid, 1, 32.0, 4.0);
  const auto* rs = s.findRegion(tid, ridS);
  TEST_ASSERT(rs != nullptr);
  TEST_ASSERT(rs->notes.empty());

  // Unknown track: no region, no crash.
  TEST_ASSERT(misuc::project::materializePatternRegion(
                  s, "no-such-track", 1, 0.0, 4.0)
                  .empty());
}

TEST_CASE(session_channel_rack_empty_is_valid) {
  Session s;
  TEST_ASSERT(s.channelRack().channels.empty());
  TEST_ASSERT(s.channelRack().activePattern == 0);
  TEST_ASSERT(s.channelRack().swing == 0);
  const std::string json = s.toJson();
  const Session loaded = Session::fromJson(json);
  TEST_ASSERT(loaded.channelRack().channels.empty());
}

TEST_CASE(session_channel_rack_legacy_missing_is_valid) {
  // A v3 document that lacks the channelRack key must load with defaults.
  const std::string v3 = R"({"format":"misuc.song","schemaVersion":3,"app":"misuc","metadata":{"name":"No Rack"},"transport":{"tempo":120}})";
  const Session loaded = Session::fromJson(v3);
  TEST_ASSERT(loaded.name() == "No Rack");
  TEST_ASSERT(loaded.channelRack().channels.empty());
}

TEST_CASE(session_channel_rack_all_banks) {
  Session s;
  auto& cr = s.channelRack();
  misuc::project::StepChannel ch;
  ch.name = "AllBanks";
  for (int b = 0; b < misuc::project::kPatternBanks; ++b) {
    ch.steps[b][0] = 100;
    ch.steps[b][1] = 60;
  }
  cr.channels.push_back(ch);
  const std::string json = s.toJson();
  const Session loaded = Session::fromJson(json);
  TEST_ASSERT(loaded.channelRack().channels.size() == 1);
  for (int b = 0; b < misuc::project::kPatternBanks; ++b) {
    TEST_ASSERT(loaded.channelRack().channels[0].steps[b][0] == 100);
    TEST_ASSERT(loaded.channelRack().channels[0].steps[b][1] == 60);
  }
}

TEST_CASE(session_channel_type_and_identity) {
  // Channel type (sampler/instrument/plugin/audio clip/...) is a persisted
  // first-class rack property, and every channel carries a stable identity
  // separate from its editable name (master spec §4/§5).
  Session s;
  auto& cr = s.channelRack();
  misuc::project::StepChannel perc;
  perc.id = "ch42";
  perc.name = "Kick";
  perc.type = misuc::project::ChannelType::Sampler;
  perc.steps[0][0] = 100;
  cr.channels.push_back(perc);
  misuc::project::StepChannel bass;
  bass.id = "ch43";
  bass.name = "Bass";
  bass.type = misuc::project::ChannelType::Instrument;
  bass.steps[0][1] = 80;
  cr.channels.push_back(bass);
  misuc::project::StepChannel lane;
  lane.id = "ch44";
  lane.name = "Filter Lane";
  lane.type = misuc::project::ChannelType::Automation;
  lane.steps[0][2] = 100;
  cr.channels.push_back(lane);

  const Session loaded = Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.channelRack().channels.size() == 3);
  TEST_ASSERT(loaded.channelRack().channels[0].id == "ch42");
  TEST_ASSERT(loaded.channelRack().channels[0].type ==
              misuc::project::ChannelType::Sampler);
  TEST_ASSERT(loaded.channelRack().channels[1].id == "ch43");
  TEST_ASSERT(loaded.channelRack().channels[1].type ==
              misuc::project::ChannelType::Instrument);
  TEST_ASSERT(loaded.channelRack().channels[2].id == "ch44");
  TEST_ASSERT(loaded.channelRack().channels[2].type ==
              misuc::project::ChannelType::Automation);

  // Legacy documents have no id/type keys: they load with a fresh identity
  // and the sampler default, and the ids stay unique across the rack.
  const std::string legacy =
      R"({"format":"misuc.song","schemaVersion":3,"metadata":{"name":"Old"},"channelRack":{"activePattern":0,"swing":0,"channels":[{"name":"Kick","steps0":"EAAAAAAAAAAAAAAAAAAAAAAAAA=="},{"name":"Hat","steps0":"AQAAAAAAAAAAAAAAAAAAAAAA"}]}})";
  const Session legacyLoaded = Session::fromJson(legacy);
  TEST_ASSERT(legacyLoaded.channelRack().channels.size() == 2);
  TEST_ASSERT(!legacyLoaded.channelRack().channels[0].id.empty());
  TEST_ASSERT(!legacyLoaded.channelRack().channels[1].id.empty());
  TEST_ASSERT(legacyLoaded.channelRack().channels[0].id !=
              legacyLoaded.channelRack().channels[1].id);
  TEST_ASSERT(legacyLoaded.channelRack().channels[0].type ==
              misuc::project::ChannelType::Sampler);
  TEST_ASSERT(legacyLoaded.channelRack().channels[1].type ==
              misuc::project::ChannelType::Sampler);
}

TEST_CASE(session_automation_channel_makes_no_notes) {
  // Automation lanes are silent: their constant gates never materialize into
  // notes in the pattern (master spec §5 "makes no sound of its own").
  Session s;
  auto& cr = s.channelRack();
  cr.activePattern = 0;
  misuc::project::StepChannel kick;
  kick.steps[0][0] = 100;
  cr.channels.push_back(kick);
  misuc::project::StepChannel filter;
  filter.type = misuc::project::ChannelType::Automation;
  filter.steps[0][0] = 100;
  filter.steps[0][4] = 100;
  cr.channels.push_back(filter);
  TEST_ASSERT(rackPatternNotes(s, 0).size() == 1);
  TEST_ASSERT(rackPatternNotes(s, 0)[0].pitch ==
              misuc::project::rackRowPitch(0));
}

TEST_CASE(session_variable_step_count) {
  // The channel rack is not hardcoded to 16 steps (master spec §9): the
  // pattern length is a persisted, rack-wide property in 4..128 and the
  // materialized gates follow it exactly.
  Session s;
  auto& cr = s.channelRack();
  cr.activePattern = 0;
  cr.stepsPerPattern = 32;
  misuc::project::StepChannel ch;
  ch.name = "Long";
  // Gate step 0, 16, and 31 of bank 0.
  ch.steps[0][0] = 100;
  ch.steps[0][16] = 110;
  ch.steps[0][31] = 90;
  cr.channels.push_back(ch);
  const auto notes = rackPatternNotes(s, 0);
  TEST_ASSERT(notes.size() == 3);
  double offsets[3];
  for (int i = 0; i < 3; ++i) offsets[i] = notes[i].startBeats;
  bool saw0 = false, saw4 = false, saw7_75 = false;
  for (double o : offsets) {
    if (o == 0.0) saw0 = true;
    if (o == 4.0) saw4 = true;             // step 16 = beat 4
    if (o == 31 * 0.25) saw7_75 = true;    // step 31 = beat 7.75
  }
  TEST_ASSERT(saw0);
  TEST_ASSERT(saw4);
  TEST_ASSERT(saw7_75);

  // Round-trip: the length and the far gates persist.
  const Session loaded = Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.channelRack().stepsPerPattern == 32);
  TEST_ASSERT(loaded.channelRack().channels[0].steps[0][31] == 90);
  TEST_ASSERT(loaded.channelRack().channels[0].steps[0][16] == 110);

  // Legacy documents lack the key: they load with the 16-step default and
  // short lanes.
  const std::string legacy =
      R"({"format":"misuc.song","schemaVersion":3,"metadata":{"name":"Old"},"channelRack":{"activePattern":0,"swing":0,"channels":[{"name":"Kick","steps0":"EAAAAAAAAAAAAAAAAAAAAAAAAA=="}]}})";
  const Session legacyLoaded = Session::fromJson(legacy);
  TEST_ASSERT(legacyLoaded.channelRack().stepsPerPattern == 16);
  TEST_ASSERT(legacyLoaded.channelRack().channels[0].steps[0][0] == 16);

  // Clamping: out-of-range lengths normalize into 4..128 on load.
  Session s2;
  s2.channelRack().stepsPerPattern = 3;
  const Session s2Loaded = Session::fromJson(s2.toJson());
  TEST_ASSERT(s2Loaded.channelRack().stepsPerPattern == 4);
  Session s3;
  s3.channelRack().stepsPerPattern = 512;
  const Session s3Loaded = Session::fromJson(s3.toJson());
  TEST_ASSERT(s3Loaded.channelRack().stepsPerPattern == 128);
}
// ── Phase N: fx control-port automation lanes ─────────────────────────────

// A lane may target an fx control port via "fx:<chain>:<port>". The resting
// value overload seeds its endpoints at the caller's value (a non-unity native
// control range like the EQ's Hz cutoff has no universal neutral), and the lane
// survives the JSON round-trip with its param + arm flag intact.
TEST_CASE(session_fx_automation_lane_rests_and_roundtrips) {
  Session s;
  s.setTempo(120.0);
  const std::string tid = s.addTrack("FX Auto");
  const std::string fxId = s.addFx(tid, misuc::engine::kBuiltinEqUri);
  TEST_ASSERT(!fxId.empty());
  const std::string laneId = s.addAutomationLane(tid, "fx:0:1", 420.0);
  TEST_ASSERT(!laneId.empty());
  const misuc::project::AutomationLane* lane = s.findAutomationLane(tid, laneId);
  TEST_ASSERT(lane != nullptr);
  TEST_ASSERT(lane->param == "fx:0:1");
  TEST_ASSERT(lane->points.front().value == 420.0);
  TEST_ASSERT(lane->points.back().value == 420.0);
  // Re-fetch non-const to arm the lane (additive post-v3 member).
  if (misuc::project::AutomationLane* mut =
          s.findAutomationLane(tid, laneId))
    mut->armed = true;
  const Session loaded = Session::fromJson(s.toJson());
  const misuc::project::Track* tl = loaded.findTrack(tid);
  TEST_ASSERT(tl != nullptr);
  bool saw = false;
  for (const auto& l : tl->automation)
    if (l.id == laneId) {
      saw = l.param == "fx:0:1" && l.armed && !l.points.empty() &&
            l.points.front().value == 420.0 && l.points.back().value == 420.0;
    }
  TEST_ASSERT(saw);
  // The two-argument form still defaults: pan rests at centre, others at unity.
  const std::string plain = s.addAutomationLane(tid);
  const misuc::project::Track* t2 = s.findTrack(tid);
  for (const auto& l : t2->automation)
    if (l.id == plain) {
      TEST_ASSERT(l.points.front().value == 1.0);
    }
}

// Removing an fx insert must keep the automation lanes consistent: a lane aimed
// at the removed slot disappears, and one aimed at a later slot shifts back by
// one (ci--) so it keeps automating the same plugin that now sits lower.
TEST_CASE(session_remove_fx_reindexes_automation_lanes) {
  Session s;
  s.setTempo(120.0);
  const std::string tid = s.addTrack("FX Reindex");
  const std::string fx0 = s.addFx(tid, misuc::engine::kBuiltinEqUri);
  const std::string fx1 = s.addFx(tid, misuc::engine::kBuiltinEqUri);
  const std::string fx2 = s.addFx(tid, misuc::engine::kBuiltinEqUri);
  const std::string laneB0 = s.addAutomationLane(tid, "fx:0:1", 300.0);
  const std::string laneB1 = s.addAutomationLane(tid, "fx:1:3", 900.0);
  const std::string laneB2 = s.addAutomationLane(tid, "fx:2:5", 2000.0);
  const std::string gainLane = s.addAutomationLane(tid, "gain");

  // Remove the middle insert (chain position 1): "fx:1:3" (the removed slot)
  // is dropped, "fx:2:5" shifts to "fx:1:5", and everything else is untouched.
  TEST_ASSERT(s.removeFx(tid, fx1));
  const misuc::project::Track* t = s.findTrack(tid);
  TEST_ASSERT(t != nullptr);
  bool sawB0 = false, sawB1 = false, sawB2 = false, sawGain = false;
  for (const auto& l : t->automation) {
    if (l.id == laneB0) {
      sawB0 = true;
      TEST_ASSERT(l.param == "fx:0:1");
    } else if (l.id == laneB1) {
      sawB1 = true; // must never survive: it targeted the removed insert
    } else if (l.id == laneB2) {
      sawB2 = true;
      TEST_ASSERT(l.param == "fx:1:5");
    } else if (l.id == gainLane) {
      sawGain = true;
      TEST_ASSERT(l.param == "gain");
    } else {
      TEST_ASSERT(false);
    }
  }
  TEST_ASSERT(sawB0 && !sawB1 && sawB2 && sawGain);
}

// ── Phase N ends ──

TEST_CASE(session_automation_lanes_roundtrip_params_and_arm) {
  Session s;
  s.setTempo(120.0);
  const std::string tid = s.addTrack("Auto");
  // A pan lane (endpoints at centre 0.0) and a volume lane (unity 1.0); the
  // volume lane is armed for live recording; the gain-like lane is written
  // with explicit points. The lanes must survive save/reload with their param
  // names, point values, muting and arm flags intact.
  const std::string panLane = s.addAutomationLane(tid, "pan");
  const std::string volLane = s.addAutomationLane(tid, "volume");
  misuc::project::AutomationLane* vol = s.findAutomationLane(tid, volLane);
  TEST_ASSERT(vol != nullptr);
  vol->armed = true;
  vol->points = {{0.0, 0.0}, {2.0, 0.8}, {4.0, 1.0}};
  misuc::project::AutomationLane* pan = s.findAutomationLane(tid, panLane);
  TEST_ASSERT(pan != nullptr);
  // Pan endpoints default to the resting value (centre = 0.0), not unity.
  TEST_ASSERT(pan->points.front().value == 0.0);
  TEST_ASSERT(pan->points.back().value == 0.0);

  const std::string json = s.toJson();
  const Session loaded = Session::fromJson(json);
  const misuc::project::Track* tl = loaded.findTrack(tid);
  TEST_ASSERT(tl != nullptr);
  bool sawPan = false, sawVol = false;
  for (const auto& l : tl->automation) {
    if (l.param == "pan" && l.id == panLane)
      sawPan = true;
    if (l.param == "volume" && l.id == volLane) {
      sawVol = true;
      TEST_ASSERT(l.armed == true);
      TEST_ASSERT(l.points.size() == 3);
      TEST_ASSERT(l.points[1].timeBeats == 2.0 && l.points[1].value == 0.8);
    }
  }
  TEST_ASSERT(sawPan && sawVol);
  // A legacy document (no "armed" key) loads with wired drives un-armed.
  const std::string legacy = R"({"format":"misuc.song","schemaVersion":3,"metadata":{"name":"x"},"transport":{"tempo":120},"tracks":[{"id":"tA","name":"a","notes":[],"automation":[{"id":"la","param":"gain","muted":false,"points":[{"timeBeats":0,"value":1}, {"timeBeats":4,"value":0.5}]}]}]})";
  const Session old = Session::fromJson(legacy);
  const misuc::project::Track* ta = old.findTrack("tA");
  TEST_ASSERT(ta != nullptr);
  TEST_ASSERT(ta->automation.size() == 1);
  TEST_ASSERT(ta->automation[0].armed == false);
  TEST_ASSERT(std::fabs(ta->automation[0].points[1].value - 0.5) < 1e-9);
}
