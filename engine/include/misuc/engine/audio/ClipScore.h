#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/dsp/ClipPlayer.h>
#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/project/Session.h>

namespace misuc::engine {

// Flattens a Session's audio clips into an immutable clip playback snapshot:
// every clip of every non-muted track is decoded from its source file,
// resampled layout captured, and gain/fades/copy metadata baked into a
// dsp::PlaybackClip. Sample data is concatenated into `storage` and each clip
// references into it, so the pair can be handed to AudioEngine::setClips.
//
// `err` accumulates a warning (first failure) but does not abort other clips —
// a missing asset degrades to a silent gap instead of killing the score.
inline bool buildClipScore(const misuc::project::Session& session,
                           std::vector<dsp::PlaybackClip>& clips,
                           std::vector<float>& storage,
                           std::string& err) noexcept {
  clips.clear();
  storage.clear();
  err.clear();

  size_t trackIndex = 0;
  for (const misuc::project::Track& t : session.tracks()) {
    if (t.clips.empty()) {
      ++trackIndex;
      continue;
    }
    for (const misuc::project::AudioClip& c : t.clips) {
      if (c.sourceFile.empty() || c.durationBeats <= 0.0)
        continue;
      misuc::audio::AudioBuffer buf;
      std::string readErr;
      if (!misuc::audio::readAudioFile(c.sourceFile, buf, readErr)) {
        if (err.empty())
          err = readErr;
        continue;
      }
      if (buf.frames == 0 || buf.channels == 0 || buf.sampleRate <= 0.0)
        continue;
      dsp::PlaybackClip pc;
      pc.channels = buf.channels;
      pc.frames = static_cast<uint32_t>(std::min<uint64_t>(
          buf.frames, static_cast<uint64_t>(UINT32_MAX)));
      pc.sampleRate = buf.sampleRate;
      pc.startBeats = c.startBeats;
      pc.offsetBeats = c.clipOffsetBeats;
      pc.durationBeats = c.durationBeats;
      pc.gain = std::clamp(dsp::gainFromDb(c.gainDb), 0.f, 8.f);
      pc.fadeInBeats = c.fadeInBeats;
      pc.fadeOutBeats = c.fadeOutBeats;
      pc.track = static_cast<uint32_t>(trackIndex);

      // Copy this clip's channels*frames interleaved samples into storage.
      const size_t first = storage.size();
      const uint64_t total =
          static_cast<uint64_t>(pc.frames) * static_cast<uint64_t>(pc.channels);
      storage.insert(storage.end(), buf.samples.begin(),
                     buf.samples.begin() + static_cast<ptrdiff_t>(total));
      pc.samples = storage.data() + first;
      clips.push_back(pc);
    }
    ++trackIndex;
  }

  std::sort(clips.begin(), clips.end(),
            [](const dsp::PlaybackClip& a, const dsp::PlaybackClip& b) {
              return a.startBeats < b.startBeats;
            });
  return true;
}

} // namespace misuc::engine