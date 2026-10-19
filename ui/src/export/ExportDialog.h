#pragma once

#include <QDialog>
#include <QString>
#include <misuc/engine/audio/WavFile.h>

class QCheckBox;
class QComboBox;
class QLineEdit;

namespace misuc::ui {

// Export target spans. Loop and Selection stay disabled when the session has
// no loop region / no arranged selection, so the dialog never silently bounces
// the wrong range.
enum class ExportRange { WholeSong, Loop, Selection };

// Dialog behind File > Export: container format (dynamically probed codecs),
// range, word-length dither, Phase R metadata (title/artist tags +
// per-project sample rate), and the Include Metronome option. Pure
// configuration -- the caller owns the offline render and the stream
// pause/restore dance around it. Last-used choices are remembered via
// QSettings so a repeat export starts from the previous setup.
class ExportDialog : public QDialog {
  Q_OBJECT
public:
  ExportDialog(misuc::audio::ExportFormat initialFormat, bool loopAvailable,
               bool selectionAvailable, QWidget* parent = nullptr,
               const QString& title = QString(),
               const QString& artist = QString(), uint32_t sampleRate = 0);

  misuc::audio::ExportFormat format() const;
  ExportRange range() const;
  bool dither() const; // meaningful for PCM16/FLAC/AIFF only
  bool includeMetronome() const;
  // Extension ("wav" / "flac" / "mp3" / "ogg" / "aiff") for the picked format.
  QString suffix() const;
  // Phase R metadata + per-project sample rate (0 = engine native).
  QString title() const;
  QString artist() const;
  uint32_t sampleRate() const;

private:
  void onFormatChanged();

  QComboBox* format_ = nullptr;
  QComboBox* range_ = nullptr;
  QCheckBox* dither_ = nullptr;
  QCheckBox* metronome_ = nullptr;
  QLineEdit* title_ = nullptr;
  QLineEdit* artist_ = nullptr;
  QComboBox* sampleRate_ = nullptr;
};

} // namespace misuc::ui