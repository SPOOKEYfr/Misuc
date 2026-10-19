#include "test_utils.h"

#include <string>

#include <misuc/engine/dsp/Pitch.h>

using misuc::dsp::chromaticName;
using misuc::dsp::isBlackKey;
using misuc::dsp::kMidiNoteMax;
using misuc::dsp::kMidiNoteMin;
using misuc::dsp::midiToNoteName;
using misuc::dsp::noteNameToMidi;

TEST_CASE(pitch_full_range_is_monotonic_and_unique) {
  // Every MIDI 0..127 note maps to a distinct, strictly-increasing SPN label,
  // running C-1 .. G9 with no gaps or duplicates.
  std::string prev;
  for (int n = kMidiNoteMin; n <= kMidiNoteMax; ++n) {
    const std::string label = midiToNoteName(n);
    TEST_ASSERT(!label.empty());
    TEST_ASSERT(label != prev);
    prev = label;
    TEST_ASSERT(noteNameToMidi(label) == n);
  }
  TEST_ASSERT(midiToNoteName(0) == std::string("C-1"));
  TEST_ASSERT(midiToNoteName(12) == std::string("C0"));
  TEST_ASSERT(midiToNoteName(24) == std::string("C1"));
  TEST_ASSERT(midiToNoteName(60) == std::string("C4"));
  TEST_ASSERT(midiToNoteName(127) == std::string("G9"));
}

TEST_CASE(pitch_black_key_flags) {
  TEST_ASSERT(isBlackKey(1));    // C#
  TEST_ASSERT(isBlackKey(61));   // C#4
  TEST_ASSERT(isBlackKey(70));   // A#4
  TEST_ASSERT(!isBlackKey(0));   // C-1
  TEST_ASSERT(!isBlackKey(60));  // C4
  TEST_ASSERT(!isBlackKey(69));  // A4
}

TEST_CASE(pitch_parse_names) {
  TEST_ASSERT(noteNameToMidi("C4") == 60);
  TEST_ASSERT(noteNameToMidi("C#4") == 61);
  TEST_ASSERT(noteNameToMidi("Db4") == 61);
  TEST_ASSERT(noteNameToMidi("D4") == 62);
  TEST_ASSERT(noteNameToMidi("A4") == 69);
  TEST_ASSERT(noteNameToMidi("A#4") == 70);
  TEST_ASSERT(noteNameToMidi("B4") == 71);
  TEST_ASSERT(noteNameToMidi("C5") == 72);
  TEST_ASSERT(noteNameToMidi("C-1") == 0);
  TEST_ASSERT(noteNameToMidi("G9") == 127);
  // Case-insensitive letters.
  TEST_ASSERT(noteNameToMidi("c4") == 60);
  TEST_ASSERT(noteNameToMidi("C#") == 61);  // default octave 4
  TEST_ASSERT(noteNameToMidi("a#4") == 70);
  // Enharmonics and beyond-range clamps.
  TEST_ASSERT(noteNameToMidi("B#4") == 72);  // -> C5
  TEST_ASSERT(noteNameToMidi("Cb5") == 71);  // -> B4
  TEST_ASSERT(noteNameToMidi("C10") == 127); // clamped to G9
  TEST_ASSERT(noteNameToMidi("C-10") == 0);  // clamped to C-1
  // Garbage input is rejected, never crashes.
  TEST_ASSERT(noteNameToMidi("") == -1);
  TEST_ASSERT(noteNameToMidi("H4") == -1);
  TEST_ASSERT(noteNameToMidi("C4x") == -1);
  TEST_ASSERT(noteNameToMidi("Quarter") == -1);
}

TEST_CASE(pitch_chromatic_names) {
  TEST_ASSERT(std::string(chromaticName(0)) == "C");
  TEST_ASSERT(std::string(chromaticName(7)) == "G");
  TEST_ASSERT(std::string(chromaticName(11)) == "B");
  TEST_ASSERT(std::string(chromaticName(13)) == "C#");  // wraps
}