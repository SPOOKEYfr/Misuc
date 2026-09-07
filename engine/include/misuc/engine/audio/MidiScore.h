#pragma once

#include <algorithm>
#include <vector>

#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Midi.h>
#include <misuc/engine/project/Session.h>

namespace misuc::engine {

// Flattens a Session into an immutable MIDI playback score: every note of every
// non-muted track, with velocity and per-track gain baked into the amplitude,
// sorted by start beat. Call from any non-RT thread when notes change
// (e.g. after an edit, undo/redo, open, or new).
inline void buildMidiScore(const misuc::project::Session& session,
                           std::vector<dsp::MidiNote>& out) {
  out.clear();
  for (const misuc::project::Track& t : session.tracks()) {
    if (t.muted)
      continue;
    const float trackGain =
        std::clamp(dsp::gainFromDb(t.gainDb), 0.f, 8.f);
    for (const misuc::project::NoteEvent& n : t.notes) {
      if (n.durationBeats <= 0.0)
        continue;
      dsp::MidiNote mn;
      mn.startBeats = n.startBeats;
      mn.durationBeats = n.durationBeats;
      mn.freq = dsp::midiNoteToFreq(n.pitch);
      mn.gain = (static_cast<float>(n.velocity) / 127.f) * trackGain;
      out.push_back(mn);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const dsp::MidiNote& a, const dsp::MidiNote& b) {
              return a.startBeats < b.startBeats;
            });
}

} // namespace misuc::engine