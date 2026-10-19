#pragma once

#include <climits>
#include <string>

namespace misuc::dsp {

// ---------------------------------------------------------------------------
// Centralized MIDI-note <-> scientific pitch notation mapping. *Every* note
// name / octave calculation in Misuc (Piano Roll keyboard, note labels,
// inspector, debugging) must go through these two functions so the whole app
// uses one internally-consistent convention:
//
//     MIDI 0  -> C-1
//     MIDI 12 -> C0
//     MIDI 24 -> C1
//     MIDI 60 -> C4   (middle C, standard SPN)
//     MIDI 127-> G9
//
// Octave = (note / 12) - 1, so C-1 min and G9 max for the full 0..127 range.
// ---------------------------------------------------------------------------

inline constexpr int kMidiNoteMin = 0;
inline constexpr int kMidiNoteMax = 127;

// Chromatic scale degree (0..11) -> SPN letter name (C, C#, D, ..., B).
inline const char* chromaticName(int degree) {
    static const char* kNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                   "F#", "G",  "G#", "A",  "A#", "B"};
    return kNames[((degree % 12) + 12) % 12];
}

// True for the SPN accidental degrees (C#/D#/F#/G#/A#).
inline bool isBlackKey(int midiNote) noexcept {
    const int c = ((midiNote % 12) + 12) % 12;
    return c == 1 || c == 3 || c == 6 || c == 8 || c == 10;
}

// MIDI 0..127 -> "C-1".."G9". Out-of-range notes are clamped to the valid
// range so callers can never produce a malformed or non-monotonic label.
inline std::string midiToNoteName(int midiNote) {
    const int clamped = midiNote < kMidiNoteMin ? kMidiNoteMin
                        : midiNote > kMidiNoteMax ? kMidiNoteMax
                                                  : midiNote;
    const int octave = clamped / 12 - 1;  // C-1 at MIDI 0
    return std::string(chromaticName(clamped)) + std::to_string(octave);
}

// "C-1".."G9" (case-insensitive, optional '#'/'b') -> MIDI note 0..127.
// Returns -1 when the name is not a parseable chromatic note, and clamps the
// resulting pitch to 0..127 (e.g. "C10" -> 127, "C-10" -> 0). "B#" parses as
// C of the next octave; "Cb" as B of the previous octave.
inline int noteNameToMidi(const std::string& name) {
    if (name.empty())
        return -1;
    size_t i = 0;
    char letter = static_cast<char>(toupper(static_cast<unsigned char>(name[i])));
    int step = -1;
    switch (letter) {
        case 'C': step = 0; break;
        case 'D': step = 2; break;
        case 'E': step = 4; break;
        case 'F': step = 5; break;
        case 'G': step = 7; break;
        case 'A': step = 9; break;
        case 'B': step = 11; break;
        default: return -1;
    }
    ++i;
    // Optional accidental: one or more '#' (sharp) or 'b' (flat).
    int accidental = 0;
    if (i < name.size() && (name[i] == '#' || name[i] == 'b' ||
                            name[i] == 's' || name[i] == 'f')) {
        char acc = name[i];
        while (i < name.size() &&
               (name[i] == '#' || name[i] == 'b' || name[i] == 's' ||
                name[i] == 'f')) {
            if (acc == '#' || acc == 's')
                ++accidental;
            else if (acc == 'b' || acc == 'f')
                --accidental;
            ++i;
        }
    }
    // Optional octave: -n, n, or +n. Missing octave defaults to octave 4
    // (the octave containing middle C), matching common usage of bare names.
    int octave = 4;
    if (i < name.size()) {
        bool neg = false;
        size_t j = i;
        if (name[j] == '-') { neg = true; ++j; }
        else if (name[j] == '+') { ++j; }
        if (j < name.size()) {
            int value = 0;
            const size_t digitStart = j;
            while (j < name.size() && name[j] >= '0' && name[j] <= '9') {
                value = value * 10 + (name[j] - '0');
                if (value > 128)
                    return -1;  // absurd octave; not a note name
                ++j;
            }
            if (j > digitStart) {
                octave = neg ? -value : value;
                i = j;
            }
        }
        if (i < name.size())
            return -1;  // trailing garbage
    }
    const int midi = (octave + 1) * 12 + step + accidental;
    return midi < kMidiNoteMin ? kMidiNoteMin
           : midi > kMidiNoteMax ? kMidiNoteMax
                                 : midi;
}

}  // namespace misuc::dsp