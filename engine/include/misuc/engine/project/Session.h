#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <misuc/engine/core/Json.h>

namespace misuc::project {

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
// playback. `sourceFile` is the persistent reference to the audio asset; the
// session serializes it verbatim so projects keep working after a reload.
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

// A track holds note events, audio clips, and mixer-ish state.
struct Track {
  std::string id;
  std::string name;
  bool muted = false;
  float gainDb = 0.0f;
  std::vector<NoteEvent> notes;
  std::vector<AudioClip> clips;

  bool operator==(const Track&) const = default;
};

// Versioned project document. The .misuc format is JSON with an explicit
// schema version so future migrations are possible. v2 adds tracks with MIDI
// notes; the loader still accepts v1 documents (empty track list).
class Session {
public:
  Session();

  std::string name() const { return name_; }
  void setName(std::string name) { name_ = std::move(name); }

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

  // Loop region (beats) persisted with the project.
  bool loopMode() const { return loopMode_; }
  double loopStart() const { return loopStartBeats_; }
  double loopEnd() const { return loopEndBeats_; }
  void setLoop(bool on, double startBeats, double endBeats) {
    loopMode_ = on && endBeats > startBeats;
    loopStartBeats_ = startBeats;
    loopEndBeats_ = endBeats;
  }

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

  // Serialize to .misuc text.
  std::string toJson() const;

  // Load from .misuc text. Throws std::runtime_error on malformed input.
  static Session fromJson(const std::string& text);

private:
  std::string newTrackId();
  std::string newClipId();

  std::string name_ = "Untitled";
  double tempo_ = 120.0;
  int tsNum_ = 4;
  int tsDen_ = 4;
  uint32_t sampleRate_ = 48000;
  bool loopMode_ = false;
  double loopStartBeats_ = 0.0;
  double loopEndBeats_ = 4.0;
  std::vector<Track> tracks_;
  uint32_t trackCounter_ = 0;
  uint32_t clipCounter_ = 0;
};

} // namespace misuc::project