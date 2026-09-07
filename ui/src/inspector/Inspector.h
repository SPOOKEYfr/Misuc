#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QVBoxLayout;
class QGridLayout;

namespace misuc::ui {

// Inspector workspace (master spec §29): a live context panel bound to the
// current selection. It renders a property grid for the selected track, audio
// clip or MIDI region and pushes edits back through callbacks. No selected
// object → a neutral workspace overview.
class Inspector : public QWidget {
  Q_OBJECT
public:
  explicit Inspector(QWidget* parent = nullptr);

  void showTrack(int row, const QString& name, bool muted, bool solo,
                 float gainDb, float pan);
  void showAudioClip(const QString& name, const QString& source,
                     double startBeats, double durationBeats, float gainDb,
                     double clipOffsetBeats);
  void showMidiRegion(int noteCount, double startBeats, double durationBeats);
  void showNote(int pitch, float velocity, double startBeats,
                double durationBeats);
  void showEmpty(const QString& workspaceHint);
  void clear();

signals:
  void editRequest(const QString& property, const QString& value);
  void selectionChanged();

protected:
  void rebuildGrid();

private:
  void addRow(const QString& label, const QString& hint = QString());

  QVBoxLayout* layout_ = nullptr;
  QGridLayout* grid_ = nullptr;
  QLabel* icon_ = nullptr;
  QLabel* title_ = nullptr;
};

} // namespace misuc::ui