#include "Inspector.h"

#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "Theme.h"

namespace misuc::ui {

Inspector::Inspector(QWidget* parent) : QWidget(parent) {
  setMinimumSize(230, 200);
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(10, 10, 10, 10);
  layout_->setSpacing(4);

  icon_ = new QLabel(this);
  icon_->setAlignment(Qt::AlignCenter);
  layout_->addWidget(icon_);

  title_ = new QLabel("No selection", this);
  title_->setStyleSheet(QString("color: %1; font-size: 13px; font-weight: 600;")
                            .arg(theme::kText.name()));
  title_->setAlignment(Qt::AlignCenter);
  layout_->addWidget(title_);
  layout_->addSpacing(8);

  showEmpty("Browse a workspace to inspect its selection.");
}

void Inspector::rebuildGrid() {
  if (grid_) {
    delete grid_;
    grid_ = nullptr;
  }
  grid_ = new QGridLayout;
  grid_->setContentsMargins(0, 0, 0, 0);
  grid_->setHorizontalSpacing(10);
  grid_->setVerticalSpacing(6);
  layout_->insertLayout(layout_->count(), grid_);
}

void Inspector::addRow(const QString& label, const QString& hint) {
  if (!grid_) rebuildGrid();
  int row = grid_->rowCount();
  auto* lab = new QLabel(label, this);
  lab->setStyleSheet(QString("color: %1;")
                         .arg(theme::kTextDim.name()));
  auto* val = new QLabel(hint.isEmpty() ? "-" : hint, this);
  val->setStyleSheet(QString("color: %1;")
                         .arg(hint.isEmpty() ? theme::kTextFaint.name()
                                             : theme::kText.name()));
  val->setAlignment(Qt::AlignRight);
  grid_->addWidget(lab, row, 0);
  grid_->addWidget(val, row, 1);
}

void Inspector::addTextEdit(const QString& label, const QString& value,
                            const QString& property, const QString& signalTarget) {
  if (!grid_) rebuildGrid();
  int row = grid_->rowCount();
  auto* lab = new QLabel(label, this);
  lab->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
  auto* edit = new QLineEdit(value, this);
  edit->setStyleSheet(QString("background: %1; color: %2; border: 1px solid %3;"
                              " border-radius: 3px; padding: 2px 6px;")
                          .arg(theme::kChromeRaised.name(), theme::kText.name(),
                               theme::kChromeBorder.name()));
  connect(edit, &QLineEdit::editingFinished, this,
          [this, property, edit, signalTarget] {
            if (edit->text() != editorValue_[signalTarget]) {
              editorValue_[signalTarget] = edit->text();
              emit editRequest(property, edit->text());
            }
          });
  grid_->addWidget(lab, row, 0);
  grid_->addWidget(edit, row, 1);
}

void Inspector::addToggle(const QString& label, bool on,
                          const QString& property) {
  if (!grid_) rebuildGrid();
  int row = grid_->rowCount();
  auto* lab = new QLabel(label, this);
  lab->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
  auto* btn = new QPushButton(on ? "ON" : "off", this);
  btn->setCheckable(true);
  btn->setChecked(on);
  btn->setStyleSheet(QString("QPushButton { background: %1; color: %2;"
                             " border: 1px solid %3; border-radius: 3px;"
                             " padding: 2px 6px; font-weight: 600; }"
                             " QPushButton:checked { background: %4;"
                             " color: %5; border-color: %4; }")
                         .arg(theme::kChromeRaised.name(), theme::kText.name(),
                              theme::kChromeBorder.name(), theme::kAccent.name(),
                              theme::kSurface.name()));
  connect(btn, &QPushButton::toggled, this,
          [this, property](bool on) {
            emit editRequest(property, on ? "1" : "0");
          });
  grid_->addWidget(lab, row, 0);
  grid_->addWidget(btn, row, 1);
}

void Inspector::addNumber(const QString& label, double value, double min,
                          double max, const QString& property) {
  if (!grid_) rebuildGrid();
  int row = grid_->rowCount();
  auto* lab = new QLabel(label, this);
  lab->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
  auto* spin = new QDoubleSpinBox(this);
  spin->setRange(min, max);
  spin->setDecimals(property.contains("pan") ? 2 : 1);
  spin->setSingleStep(property.contains("pan") ? 0.1 : 0.5);
  if (property.contains("gain")) spin->setSuffix(" dB");
  if (property.contains("trim") || property.contains("duration"))
    spin->setSuffix(" b");
  spin->setValue(value);
  spin->setStyleSheet(QString("background: %1; color: %2; border: 1px solid %3;"
                              " border-radius: 3px; padding: 2px 4px;")
                          .arg(theme::kChromeRaised.name(), theme::kText.name(),
                               theme::kChromeBorder.name()));
  connect(spin, &QDoubleSpinBox::editingFinished, this,
          [this, property, spin] {
            emit editRequest(property, QString::number(spin->value(), 'f', 2));
          });
  grid_->addWidget(lab, row, 0);
  grid_->addWidget(spin, row, 1);
}

void Inspector::showTrack(int row, const QString& name, bool muted, bool solo,
                          float gainDb, float pan) {
  rebuildGrid();
  title_->setText(name.isEmpty() ? QStringLiteral("[Track %1]").arg(row)
                                  : name);
  if (name.isEmpty())
    addRow("Track", QString::number(row + 1));
  else
    addTextEdit("Track", name, "track.name", "track.name");
  addToggle("Mute", muted, "track.mute");
  addToggle("Solo", solo, "track.solo");
  addNumber("Gain", gainDb, -60.0, 24.0, "track.gain");
  addNumber("Pan", static_cast<double>(pan), -1.0, 1.0, "track.pan");
}

void Inspector::showAudioClip(const QString& name, const QString& source,
                              double startBeats, double durationBeats,
                              float gainDb, double clipOffsetBeats) {
  rebuildGrid();
  title_->setText(name);
  addTextEdit("Name", name, "clip.name", "clip.name");
  addRow("Source", source);
  addRow("Start", QString::number(startBeats, 'f', 2) + " b");
  addRow("Length", QString::number(durationBeats, 'f', 2) + " b");
  addNumber("Trim-in", clipOffsetBeats, 0.0, 100000.0, "clip.trim");
  addNumber("Gain", gainDb, -60.0, 24.0, "clip.gain");
}

void Inspector::showMidiRegion(int noteCount, double startBeats,
                               double durationBeats) {
  rebuildGrid();
  title_->setText("MIDI Region");
  addRow("Notes", QString::number(noteCount));
  addRow("Start", QString::number(startBeats, 'f', 2) + " b");
  addRow("Length", QString::number(durationBeats, 'f', 2) + " b");
}

void Inspector::showNote(int pitch, float velocity, double startBeats,
                         double durationBeats) {
  rebuildGrid();
  title_->setText("Note");
  addRow("Pitch", QString::number(pitch));
  addRow("MIDI", QString::number(pitch));
  addRow("Velocity", QString::number(qRound(velocity * 127)));
  addRow("Start", QString::number(startBeats, 'f', 2) + " b");
  addRow("Length", QString::number(durationBeats, 'f', 2) + " b");
}

void Inspector::showEmpty(const QString& workspaceHint) {
  rebuildGrid();
  title_->setText("Inspector");
  addRow("Context", workspaceHint);
}

void Inspector::clear() {
  title_->setText("No selection");
  if (grid_) delete grid_;
  grid_ = nullptr;
}

} // namespace misuc::ui