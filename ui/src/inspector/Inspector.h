#pragma once

#include <QWidget>
#include <QHash>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QDoubleSpinBox;
class QVBoxLayout;
class QGridLayout;
class QComboBox;
class QScrollArea;

#include "elide/ElideLabel.h"

namespace misuc::ui {

class HintManager;

// View model for one effect insert shown in the track inspector: the display
// name, enable state and one adjustable parameter (port, native range and the
// current native value as reported by the plugin catalog).
struct FxSlotView {
  QString id;
  QString name;
  bool enabled = true;
  struct Param {
    uint32_t index = 0;
    QString label;
    double min = 0.0;
    double max = 1.0;
    double value = 0.0;
    // Whether a record-armed "fx:<chain>:<index>" automation lane exists.
    bool armed = false;
  };
  std::vector<Param> params;
};

// One entry of the discoverable effect catalog offered in the add combo.
struct FxCatalogItem {
  QString uri;
  QString name;
};

// One recorded take shown in the track inspector: name, its transport span and
// whether its clip is currently placed on the timeline. "Place" collapses the
// timeline to exactly this take; "remove" deletes the take and its clip.
struct TakeView {
  QString id;
  QString name;
  double startBeats = 0.0;
  double durationBeats = 1.0;
  bool placed = false;
};

// Inspector workspace (master spec §29): a live context panel bound to the
// current selection. It renders a property grid for the selected track, audio
// clip or MIDI region and pushes edits back through callbacks. No selected
// object → a neutral workspace overview.
class Inspector : public QWidget {
  Q_OBJECT
public:
  explicit Inspector(QWidget* parent = nullptr);

  void showTrack(int row, const QString& name, bool muted, bool solo,
                 float gainDb, float pan,
                 const std::vector<FxSlotView>& fxSlots = {},
                 const std::vector<FxCatalogItem>& catalog = {},
                 const std::vector<TakeView>& takes = {});
  void showAudioClip(const QString& name, const QString& source,
                     double startBeats, double durationBeats, float gainDb,
                     double clipOffsetBeats);
  void showMidiRegion(int noteCount, double startBeats, double durationBeats);
  void showNote(int pitch, float velocity, double startBeats,
                double durationBeats);
  void showEmpty(const QString& workspaceHint);
  void clear();

  // Registers hints for the dynamically rebuilt property grid, takes and FX
  // rack controls (they are recreated on every selection change, so each
  // rebuild re-registers them; the QPointer-keyed registry drops them
  // automatically when the old widgets are destroyed).
  void attachHints(HintManager* hints);

private:
  void addTakes(const std::vector<TakeView>& takes);

signals:
  void editRequest(const QString& property, const QString& value);
  void selectionChanged();

protected:
  void rebuildGrid();

private:
  void addRow(const QString& label, const QString& hint = QString());
  void addTextEdit(const QString& label, const QString& value,
                   const QString& property, const QString& signalTarget);
  void addToggle(const QString& label, bool on, const QString& property);
  void addNumber(const QString& label, double value, double min, double max,
                 const QString& property);
  // FX rack section rendered under the track property grid.
  void addFxRack(const std::vector<FxSlotView>& fxSlots,
                 const std::vector<FxCatalogItem>& catalog);
  void clearSection();

  QVBoxLayout* layout_ = nullptr;
  QScrollArea* scrollArea_ = nullptr;
  QWidget* scrollContent_ = nullptr;
  QVBoxLayout* scrollLayout_ = nullptr;
  QWidget* gridHost_ = nullptr;   // owns the rebuilt property grid rows
  QGridLayout* grid_ = nullptr;
  QHash<QString, QString> editorValue_; // last submitted value per property
  QLabel* icon_ = nullptr;
  ElideLabel* title_ = nullptr;
  QWidget* takesHost_ = nullptr;  // takes section (cleared on every rebuild)
  QWidget* fxHost_ = nullptr;     // FX rack section (cleared on every rebuild)
  HintManager* hints_ = nullptr;  // hint registry; null until attached
};

} // namespace misuc::ui