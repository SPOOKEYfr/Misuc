#pragma once

#include <QString>

namespace misuc::project {
class Session;
}

namespace misuc::ui {

// First-run demo: a real, playable starter project (a MIDI arrangement on
// drums/bass/keys plus one generated pad audio clip) so a fresh install opens
// a visibly structured workspace (Playlist lanes, ruler, clips; Piano Roll
// notes; audio in the Pad clip). Assets live under
// QStandardPaths::AppDataLocation/MisucDemo and are honest: every note and
// clip is session-backed and audible through the engine.
struct DemoPaths {
  QString dir;
  QString project; // abs path to the .misuc document
  QString wav;     // abs path to the generated pad audio
};

// Builds the deterministic demo Session. `padWavAbsolute` becomes the pad
// track's audio-clip source so rendering works straight after load.
misuc::project::Session buildDemoSession(const QString& padWavAbsolute);

// Generates (or refreshes) the pad WAV and the demo .misuc file under the
// AppData demo directory. Never touches user projects. Returns false with a
// human-readable `err` on failure.
bool ensureDemoProject(DemoPaths& out, QString& err);

} // namespace misuc::ui