#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <misuc/engine/core/Json.h>

namespace misuc::project {

// A recorded take: a self-contained snapshot of one input capture (mono WAV in
// the project audio/ folder), with the exact transport span the performance
// occupied. The take list lets a session hold alternates without cluttering
// the timeline; `clipId` names the clip this take materialized into (empty
// while the take stays unplaced). Takes survive save/reload like every other
// session element.
struct RecordTake {
  std::string id;
  std::string name;
  std::string sourceFile;      // WAV asset on disk
  double startBeats = 0.0;     // where the take was performed (transport span)
  double durationBeats = 1.0;  // length in beats
  float gainDb = 0.0f;
  uint32_t sampleRate = 0;
  uint16_t channels = 1;
  std::string clipId;          // the clip this take materialized, if placed

  bool operator==(const RecordTake&) const = default;
};

// A single MIDI note in a track. Time is in beats relative to the track start.
struct NoteEvent {
  uint8_t channel = 0;
  uint8_t pitch = 60;      // MIDI note number (60 = middle C)
  uint8_t velocity = 100;  // 1..127
  double startBeats = 0.0;
  double durationBeats = 1.0;

  bool operator==(const NoteEvent&) const = default;
};

// An audio region on a track's timeline, backed by a sound file on disk.
// Timeline quantities (start, duration, offset, fades) are in beats to match
// the note model; the engine resamples the source to the project rate on
// playback. `sourceFile` is the persistent reference to the audio asset. The
// in-memory session always holds absolute paths so playback works after a
// reload; MainWindow calls Session::relativizeAssets before writing a project
// to disk so the saved document stays portable (assets inside the project
// folder are stored relative to it), and Session::resolveAssets after loading
// to restore absolute paths for the engine.
struct AudioClip {
  std::string id;
  std::string name;
  std::string sourceFile;      // path to the WAV (or FLAC/Ogg) asset
  double startBeats = 0.0;     // placement on the timeline
  double durationBeats = 1.0;  // region length on the timeline
  double clipOffsetBeats = 0.0; // trim-in offset into the source (beats)
  float gainDb = 0.0f;
  double fadeInBeats = 0.0;
  double fadeOutBeats = 0.0;
  uint32_t sampleRate = 0;     // source metadata from import/record
  uint16_t channels = 0;

  bool operator==(const AudioClip&) const = default;
};

// A MIDI region on a track's timeline: a named, time-bounded block of notes.
// Notes are stored in LOCAL time (relative to the region's own start), so the
// region can be moved, resized, sliced, duplicated and pasted without touching
// the note timestamps; the engine only ever hears the flattened absolute forms
// (region.startBeats + note.startBeats). A track carries zero or more regions.
struct MidiRegion {
  std::string id;
  std::string name;
  double startBeats = 0.0;     // placement on the track timeline
  double durationBeats = 4.0;  // length on the track timeline
  std::vector<NoteEvent> notes; // local beats relative to region start

  bool operator==(const MidiRegion&) const = default;
};

// A single automation point: a value on the track timeline at timeBeats.
struct AutomationPoint {
  double timeBeats = 0.0;
  double value = 1.0;  // parameter units (gain: linear amplitude, 1.0 = unity)

  bool operator==(const AutomationPoint&) const = default;
};

// One automation lane on a track. `param` names the target ("gain" today).
// Points are kept sorted by timeBeats; the engine interpolates linearly
// between neighbours and holds the endpoints outside the curve. Automating
// track gain is real: the curve is published to the audio engine and affects
// playback and offline export through the same per-track gain stage.
struct AutomationLane {
  std::string id;
  std::string param = "gain";   // "gain" | "volume" | "pan"
  std::vector<AutomationPoint> points;
  bool muted = false;           // curve held (disabled) while true
  bool armed = false;           // live per-lane recording armed

  bool operator==(const AutomationLane&) const = default;
};

// A single channel in the step sequencer (channel rack). Each channel has a
// name, mixer state (mute/solo/volume/pan), and four 16-step gate lanes
// (one per pattern bank). The channel rack is session-persisted so it
// survives save/reload and participates in undo like every other session
// element.
constexpr int kStepCount = 16;   // default pattern length (still a common loop bound)
constexpr int kMaxStepCount = 128; // hard maximum; backing arrays use this
constexpr int kPatternBanks = 4;
constexpr int kMaxSwing = 66; // percent

// What a channel's rows actually generate/trigger. Kept as a first-class,
// persisted property so the rack records the sampler/instrument/audio-clip
// relationship the way the master spec's channel list does; the engine maps
// the type to the right sound source when a pattern materializes/auditions.
enum class ChannelType {
  Sampler,    // one-shot sample trigger (kick, snare, hat, ...)
  Instrument, // pitched generator / MIDI instrument (bass synth, piano, ...)
  Plugin,     // hosted (LV2) instrument
  AudioClip,  // references a sound file in the arrangement
  Automation, // control lane, makes no sound of its own
  Midi,       // external MIDI generator
};

// Portable, stable label used in the .misuc document and the UI.
inline const char* channelTypeName(ChannelType t) noexcept {
  switch (t) {
    case ChannelType::Sampler:    return "sampler";
    case ChannelType::Instrument: return "instrument";
    case ChannelType::Plugin:     return "plugin";
    case ChannelType::AudioClip:  return "audioClip";
    case ChannelType::Automation: return "automation";
    case ChannelType::Midi:       return "midi";
  }
  return "sampler";
}

struct StepChannel {
  // Stable internal identity, separate from the visible name: renaming the
  // channel must not change what it refers to (master spec §4). Allocated by
  // the Session (newChannelId) and persisted; legacy documents get one on load.
  std::string id;
  std::string name;
  ChannelType type = ChannelType::Sampler;
  bool muted = false;
  bool solo = false;
  float volume = 0.80f; // 0..1
  float pan = 0.0f;     // -1..1
  // Sound asset for a sampler channel: the audio file this row's gates
  // trigger. Empty for built-in instrument voices. Persisted with the channel
  // and treated as a portable project asset like clip sources (relativized on
  // save, resolved on load), so a dropped sample's reference survives
  // save/reload (master spec §4 drag→Channel Rack).
  std::string soundFile;
  // Step lanes: one 128-step row per pattern bank. Only the first
  // `ChannelRackData::stepsPerPattern` entries per bank are active; the
  // rest are unused slack that survives round-trips without loss.
  uint8_t steps[kPatternBanks][kMaxStepCount] = {};

  bool operator==(const StepChannel&) const = default;
};

// Default velocity written when a step is turned on by the UI / API.
constexpr int kStepDefaultVelocity = 100;

// Rack-wide state: the channel list, active pattern bank (0..3), the per-bank
// pattern length in steps (4..128, default 16 — the rack is not hardcoded to
// 16 steps, master spec §9), and swing amount (0..66 percent). Stored as a
// top-level element of the Session document so the channel rack survives
// save/reload.
struct ChannelRackData {
  std::vector<StepChannel> channels;
  int activePattern = 0;
  int swing = 0;
  int stepsPerPattern = kStepCount; // 4..kMaxStepCount

  bool operator==(const ChannelRackData&) const = default;
};

// ── Channel-rack kit mapping ──────────────────────────────────────────────
// The step sequencer's seeded kit implies one percussion pitch per row
// ({A1, A3, E5, A1, A4}) and every row beyond the kit falls back to A3. Live
// rack audition, Place Pattern and the playlist Paint tool all derive pitches
// from EXACTLY this mapping, so a painted pattern sounds like the virtual
// instrument it came from.
constexpr int kRackKitRows = 5;
inline double rackRowFreq(int row) noexcept {
  static const double kKit[5] = {55.0, 220.0, 660.0, 55.0, 440.0};
  return row >= 0 && row < kRackKitRows ? kKit[row] : 220.0;
}
// The MIDI note number the frequency above maps to (clamped to 0..127).
inline int rackRowPitch(int row) noexcept {
  const double f = rackRowFreq(row);
  const int midi = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(f / 440.0)));
  return std::clamp(midi, 0, 127);
}

// Forward: buses carry a post-fader fx chain (defined after FxParam/TrackFx).
struct TrackFx;

// A named output group. Tracks route into a bus (or straight to the master
// output when `Track::busId` is empty); the bus applies its own fader gain,
// pan, mute and solo to everything routed into it, so one fader controls a
// whole subgroup (drums, bass, ...). Buses are a v4 additive schema element:
// documents without a top-level "buses" array load with zero buses.
//
// Routing is a real graph (Comp19): `target` names another bus (empty =
// master), so a bus can feed a parent bus, giving nested subgroups. The
// engine renders buses in dependency order; publishing rejects cycles by
// falling the cyclic edge back to the master, and `setBusTarget` refuses to
// create a cycle at the model level. Buses also carry their own post-fader fx
// chain (`fx`), so EQ/compression can process a whole subgroup at once.
struct Bus {
  std::string id;
  std::string name;
  float faderDb = 0.0f; // -60..+12 (interpreted as linear gain)
  float pan = 0.0f;     // -1 = left, 0 = center, +1 = right
  bool muted = false;
  bool solo = false;
  // Destination bus id; empty = master output. Additive (Comp19).
  std::string target;
  std::vector<TrackFx> fx;

  bool operator==(const Bus&) const = default;
};

// One adjustable parameter of an effect insert. The value is in the plugin's
// native units; the UI maps slider positions onto the {min, max, def} range
// reported by the plugin at discovery time and the value defaults to the
// plugin's default when the slot is created.
struct FxParam {
  uint32_t index = 0; // port number inside the plugin
  float value = 1.0f; // native units

  bool operator==(const FxParam&) const = default;
};

// A single effect insert on a track's post-fader chain. `uri` names a
// discovered LV2 effect plugin (float audio/control ports only) or a built-in
// (`builtin:parametric-eq`, `builtin:sc-compressor`). Slots render in order
// after the track fader/pan/automation stage and before the meters;
// `enabled = false` bypasses the instance without re-instantiating it. FX is a
// v4 additive schema element: documents without a "fx" array load with an
// empty chain. When LV2 device support is absent the chain stays inert (the
// session still round-trips) — hosting is gated by build-time discovery.
//
// `sidechainSource` (Comp19) names the bus whose post-fader signal feeds the
// insert's key input (used by builtin:sc-compressor for sidechain ducking).
// It round-trips generically on tracks and buses; the engine honors it on bus
// chains (the common kick-drives-* patch); track-chain inserts carry it through
// the document but the engine does not feed it there (honest limitation,
// documented). Empty = the insert keys its own signal.
struct TrackFx {
  std::string id;
  std::string uri;
  bool enabled = true;
  std::vector<FxParam> params;
  std::string sidechainSource;

  bool operator==(const TrackFx&) const = default;
};

// A parallel send (master spec §21): the source (a track or a bus) stays
// audible through its own route while a post-fader tap, scaled by `levelDb`,
// also feeds `targetBus` — the classic reverb/delay/duck aux bus. Sends do
// not disconnect the source: both the dry route and the send sound. The tap
// is post-fader, after inserts; it follows the source's mute/solo so a muted
// or solo-gated source sends silence. `targetBus` must exist; sends to a
// missing bus are ignored at render time (persisted anyway for re-pointing).
// Additive (Comp19): documents without a top-level "sends" array load with no
// sends.
struct Send {
  std::string id;
  // Exactly one of `sourceTrack` / `sourceBus` is set; the other is empty.
  std::string sourceTrack;
  std::string sourceBus;
  std::string targetBus; // required, resolved like Track::busId
  float levelDb = -6.0f; // -60..+12 (interpreted as linear gain)
  bool enabled = true;

  bool operator==(const Send&) const = default;
};

// A track holds note events, audio clips, MIDI regions, and mixer state. Mixer
// parameters (gainDb, pan, mute, solo) are model-owned so they can be
// automated and persisted independently of any UI. Pan is in [-1,+1]
// (L..R, 0 = center).
//
// `notes` is the legacy whole-track form: it survives for backward
// compatibility with older documents and schedules that wrote absolute beats.
// A track with at least one region ignores legacy notes during playback (see
// engine::buildMidiScore); old projects without regions are migrated into a
// single region at load.
struct Track {
  std::string id;
  std::string name;
  uint32_t color = 0;         // ARGB color for track header; 0 = theme default
  bool muted = false;
  bool solo = false;
  bool recordArm = false; // input-recording armed (records onto this track)
  float gainDb = 0.0f;
  float pan = 0.0f; // -1 = left, 0 = center, +1 = right
  // Destination output group; empty = master. Stored as an id (not a vector
  // index) so the reference survives reordering and survives load.
  std::string busId;
  std::vector<TrackFx> fx;
  std::vector<NoteEvent> notes;
  std::vector<MidiRegion> regions;
  std::vector<AudioClip> clips;
  std::vector<AutomationLane> automation;
  std::vector<RecordTake> takes;

  bool operator==(const Track&) const = default;
};

// Versioned project document. The .misuc format is JSON with an explicit
// schema version so future migrations are possible. v2 added tracks with MIDI
// notes; the loader still accepts v1 documents (empty track list). v3 adds a
// `format` marker, real created/modified UTC stamps, and rejects documents
// from a future schema version with a clear error instead of mis-reading.
class Session {
public:
  Session();

  std::string name() const { return name_; }
  void setName(std::string name) { name_ = std::move(name); }

  // Export metadata (Phase R): artist of the project, persisted and written to
  // the audio file's Artist tag on export (RIFF INFO / Vorbis comment).
  const std::string& artist() const { return artist_; }
  void setArtist(std::string artist) { artist_ = std::move(artist); }

  // RFC-3339 UTC stamps persisted with the project. Empty for legacy
  // documents that predate v3.
  const std::string& createdUtc() const { return createdUtc_; }
  const std::string& modifiedUtc() const { return modifiedUtc_; }
  void setCreatedUtc(std::string s) { createdUtc_ = std::move(s); }
  void setModifiedUtc(std::string s) { modifiedUtc_ = std::move(s); }
  // Sets createdUtc to "now" (UTC) when it is still empty; returns whether it
  // changed so callers can avoid needless dirtying.
  bool ensureCreatedUtc();
  // Sets modifiedUtc to "now" (UTC).
  void touchModified();

  double tempo() const { return tempo_; }
  void setTempo(double tempo) { tempo_ = tempo; }

  int timeSigNumerator() const { return tsNum_; }
  int timeSigDenominator() const { return tsDen_; }
  void setTimeSignature(int num, int den) {
    tsNum_ = num;
    tsDen_ = den;
  }

  uint32_t sampleRate() const { return sampleRate_; }
  void setSampleRate(uint32_t sr) { sampleRate_ = sr; }

  // Buses --------------------------------------------------------------
  const std::vector<Bus>& buses() const { return buses_; }
  std::vector<Bus>& buses() { return buses_; }

  // Appends a bus and returns its stable id. Tracks keep their existing
  // routing; nothing is routed into the new bus until a track points at it.
  std::string addBus(std::string name);
  // Deletes the bus and re-routes every track that pointed at it back to the
  // master output. Returns false when no such bus exists.
  bool removeBus(const std::string& busId);
  Bus* findBus(const std::string& busId);
  const Bus* findBus(const std::string& busId) const;

  // Routing graph (Comp19): routes `busId` into `targetBusId` (empty = master
  // output). Refuses to create a cycle (a bus feeding itself, directly or
  // transitively) — returns false and leaves routing unchanged. Returns false
  // when either id does not exist.
  bool setBusTarget(const std::string& busId, const std::string& targetBusId);

  // Sends (Comp19, master spec §21): parallel routing from a track or bus into
  // a target bus. The source keeps its own route; the send adds a post-fader
  // tap scaled by `levelDb`.
  const std::vector<Send>& sends() const { return sends_; }
  std::vector<Send>& sends() { return sends_; }
  // Creates a send (pass exactly one of `sourceTrackId`/`sourceBusId`) and
  // returns its id, or "" when the source or target does not exist.
  std::string addSend(const std::string& sourceTrackId,
                      const std::string& sourceBusId,
                      const std::string& targetBusId, float levelDb);
  bool removeSend(const std::string& sendId);
  Send* findSend(const std::string& sendId);
  const Send* findSend(const std::string& sendId) const;

  // FX inserts. Adds an effect slot to a track's chain (defaulting its params
  // to the plugin's defaults later, at hosting time) and returns its id, or ""
  // if the track was not found.
  std::string addFx(const std::string& trackId, std::string uri);
  bool removeFx(const std::string& trackId, const std::string& fxId);
  // Adds an effect insert to a bus's post-fader chain and returns its id, or
  // "" when the bus was not found. `sidechainSource` (a bus id or empty) is
  // stored with the insert so sidechain-keyed inserts round-trip.
  std::string addBusFx(const std::string& busId, std::string uri,
                       std::string sidechainSource = std::string());
  bool removeBusFx(const std::string& busId, const std::string& fxId);

  // Loop region (beats) persisted with the project.
  bool loopMode() const { return loopMode_; }
  double loopStart() const { return loopStartBeats_; }
  double loopEnd() const { return loopEndBeats_; }
  void setLoop(bool on, double startBeats, double endBeats) {
    loopMode_ = on && endBeats > startBeats;
    loopStartBeats_ = startBeats;
    loopEndBeats_ = endBeats;
  }

  // Master bus gain (dB) persisted with the project. v3 schema: additive
  // transport.masterGainDb with a 0.0 dB default, so old documents still load.
  float masterGainDb() const { return masterGainDb_; }
  void setMasterGainDb(float db) { masterGainDb_ = db; }

  // Metronome (additive v4): persisted per-project state so the toolbar
  // toggle, volume, and count-in survive round-trips.  Off by default so
  // legacy documents load without unexpected clicks.
  bool metronomeOn() const { return metronomeOn_; }
  void setMetronomeOn(bool on) { metronomeOn_ = on; }
  float metronomeVolume() const { return metronomeVolume_; }
  void setMetronomeVolume(float v) {
    metronomeVolume_ = (v < 0.f ? 0.f : (v > 1.f ? 1.f : v));
  }
  float countInBeats() const { return countInBeats_; }
  void setCountInBeats(float b) { countInBeats_ = b; }

  // Effective end of the arrangement in beats: the latest point any audio
  // clip, MIDI region (including notes that overhang the region's duration),
  // or legacy whole-track note reaches. Returns 0.0 when the project is empty.
  // The transport stops at this beat so playback never runs past the content.
  double contentEndBeats() const;

  // Tracks ----------------------------------------------------------------
  const std::vector<Track>& tracks() const { return tracks_; }
  std::vector<Track>& tracks() { return tracks_; }

  // Adds a track and returns its stable id.
  std::string addTrack(std::string name);
  bool removeTrack(const std::string& id);
  Track* findTrack(const std::string& id);
  const Track* findTrack(const std::string& id) const;

  // Adds an audio clip to the given track and returns its stable id. The clip
  // id is generated here (ignoring any caller-set id) so it stays unique.
  std::string addClip(const std::string& trackId, AudioClip clip);

  // Record arming -------------------------------------------------------
  // Arms/disarms a track for input recording (false when the track does not
  // exist). Armed tracks are the record target and hold the take list.
  bool setRecordArm(const std::string& trackId, bool on);
  bool isRecordArmed(const std::string& trackId) const;

  // Takes ---------------------------------------------------------------
  // Appends a take to a track (fresh unique id, caller-set id ignored).
  // Returns the new id, or empty when the track does not exist.
  std::string addTake(const std::string& trackId, RecordTake take);
  const std::vector<RecordTake>& takes(const std::string& trackId) const;
  RecordTake* findTake(const std::string& trackId, const std::string& takeId);
  const RecordTake* findTake(const std::string& trackId,
                             const std::string& takeId) const;
  bool removeTake(const std::string& trackId, const std::string& takeId);

  // Regions ---------------------------------------------------------------
  // Adds a MIDI region to the given track and returns its stable id (any
  // caller-set id is replaced). `startBeats`/`durationBeats` are the region's
  // placement on the track timeline; its notes use local coordinates. Returns
  // an empty string when the track does not exist.
  std::string addRegion(const std::string& trackId, std::string name,
                        double startBeats, double durationBeats);
  MidiRegion* findRegion(const std::string& trackId, const std::string& regionId);
  const MidiRegion* findRegion(const std::string& trackId,
                               const std::string& regionId) const;
  bool removeRegion(const std::string& trackId, const std::string& regionId);
  // Duplicates a region (fresh unique id, copied notes) starting one region
  // length after the source by default; returns the new id (empty on failure).
  std::string duplicateRegion(const std::string& trackId, const std::string& regionId);

  // Automation ------------------------------------------------------------
  // Creates an automation lane on the track with two endpoints at the param's
  // resting value, so an edit always has a visible curve to move. Returns the
  // new lane id (empty when the track does not exist). `restingValue` is the
  // native value the endpoints sit at; the two-argument form picks the param's
  // default (gain/volume 1.0, pan 0.0) — fx "fx:<chain>:<port>" lanes should
  // pass the control's current value so the lane starts where the sound is.
  std::string addAutomationLane(const std::string& trackId,
                                const std::string& param = "gain");
  std::string addAutomationLane(const std::string& trackId,
                                const std::string& param, double restingValue);
  AutomationLane* findAutomationLane(const std::string& trackId,
                                     const std::string& laneId);
  const AutomationLane* findAutomationLane(const std::string& trackId,
                                           const std::string& laneId) const;
  bool removeAutomationLane(const std::string& trackId,
                            const std::string& laneId);

  // FX inserts (declared above, next to the buses/sends section).
  // Channel rack (step sequencer) ----------------------------------------
  const ChannelRackData& channelRack() const { return channelRack_; }
  ChannelRackData& channelRack() { return channelRack_; }
  void setChannelRack(ChannelRackData rack) { channelRack_ = std::move(rack); }

  // Allocates a stable channel identity (used for channels missing one, e.g.
  // legacy documents and the seeded starter kit). Does not touch the rack.
  std::string newChannelId();

  // Serialize to .misuc text.
  std::string toJson() const;

  // Load from .misuc text. Throws std::runtime_error on malformed input.
  static Session fromJson(const std::string& text);

  // Portable asset paths. Converts every absolute clip path that lives under
  // `baseDir` into a path relative to it, so the serialized project still
  // references its audio after the folder moves. Paths outside `baseDir`
  // (and paths already relative) are left untouched. Mutates this session so
  // callers can relativize a copy to keep the live session absolute.
  void relativizeAssets(const std::string& baseDir);

  // Inverse of relativizeAssets: joins any relative clip path against
  // `baseDir` to restore absolute paths for playback. Absolute paths (and
  // already-resolved paths such as `~` references) are left untouched.
  void resolveAssets(const std::string& baseDir);

private:
  std::string newTrackId();
  std::string newClipId();
  std::string newTakeId();
  std::string newRegionId();
  std::string newAutomationId();
  std::string newBusId();
  std::string newFxId();
  std::string newSendId();

  std::string name_ = "Untitled";
  std::string artist_;
  std::string createdUtc_;
  std::string modifiedUtc_;
  double tempo_ = 120.0;
  int tsNum_ = 4;
  int tsDen_ = 4;
  uint32_t sampleRate_ = 48000;
  bool loopMode_ = false;
  double loopStartBeats_ = 0.0;
  double loopEndBeats_ = 4.0;
  float masterGainDb_ = 0.0f;
  bool metronomeOn_ = false;
  float metronomeVolume_ = 0.9f;
  float countInBeats_ = 2.0f;
  std::vector<Track> tracks_;
  std::vector<Bus> buses_;
  ChannelRackData channelRack_;
  uint32_t trackCounter_ = 0;
  uint32_t clipCounter_ = 0;
  uint32_t takeCounter_ = 0;
  uint32_t regionCounter_ = 0;
  uint32_t automationCounter_ = 0;
  uint32_t busCounter_ = 0;
  uint32_t fxCounter_ = 0;
  uint32_t sendCounter_ = 0;
  uint32_t channelCounter_ = 0;
  std::vector<Send> sends_;
};

// The notes a pattern bank materializes into (16th-note gates, pitch per
// rackRowPitch). Muted channels are skipped; when any channel is soloed, only
// the soloed channels sound — mirroring live rack playback. Uses the bank
// passed in, or the rack's active bank when it is out of range.
std::vector<NoteEvent> rackPatternNotes(const Session& session, int bank);

// Materializes the rack's active pattern as a named "Pattern N" MIDI region on
// a track: a time-bounded block at [startBeats, startBeats+durationBeats)
// holding the bank's notes. Returns the new region id (empty on failure). This
// is the single place a pattern becomes an arrangement block — used by the
// channel rack's "Place Pattern" and the playlist Paint tool alike.
std::string materializePatternRegion(Session& session, const std::string& trackId,
                                     int bank, double startBeats,
                                     double durationBeats);

} // namespace misuc::project