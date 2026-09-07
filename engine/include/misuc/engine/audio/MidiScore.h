#pragma once

#include <algorithm>
#include <vector>

#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Midi.h>
#include <misuc/engine/project/Session.h>

namespace misuc::engine {

// Flattens a Session into an immutable MIDI playback score. Every note is
// tagged with its owning track index `track`; the note carries only its
// velocity as amplitude. Per-track gain, pan, mute and solo are applied by the
// audio mixer at render time (so they can change without re-scanning notes).
// Notes are sorted by start beat. Call from any non-RT thread.
inline void buildMidiScore(const misuc::project::Session& session,
                           std::vector<dsp::MidiNote>& out) {
  out.clear();
  size_t trackIndex = 0;
  for (const misuc::project::Track& t : session.tracks()) {
    for (const misuc::project::NoteEvent& n : t.notes) {
      if (n.durationBeats <= 0.0)
        continue;
      dsp::MidiNote mn;
      mn.startBeats = n.startBeats;
      mn.durationBeats = n.durationBeats;
      mn.freq = dsp::midiNoteToFreq(n.pitch);
      mn.gain = static_cast<float>(n.velocity) / 127.f;
      mn.track = static_cast<uint32_t>(trackIndex);
      out.push_back(mn);
    }
    ++trackIndex;
  }
  std::sort(out.begin(), out.end(),
            [](const dsp::MidiNote& a, const dsp::MidiNote& b) {
              return a.startBeats < b.startBeats;
            });
}

} // namespace misuc::engine