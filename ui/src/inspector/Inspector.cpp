#include "Inspector.h"

#include <QGridLayout>
#include <QLabel>
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

void Inspector::showTrack(int row, const QString& name, bool muted, bool solo,
                          float gainDb, float pan) {
  rebuildGrid();
  title_->setText(name.isEmpty() ? QStringLiteral("[Track %1]").arg(row)
                                  : name);
  QString panT;
  if (pan < -0.01f) panT = pan * 100 < -1 ? QString("L%1").arg(int(-pan * 100))
                                          : "L";
  else if (pan > 0.01f) panT = QString("R%1").arg(int(pan * 100));
  else panT = "C";
  addRow("Track", name.isEmpty() ? QString::number(row + 1) : name);
  addRow("Mute", muted ? "MUTED" : "on");
  addRow("Solo", solo ? "SOLO" : "off");
  addRow("Gain", QString::number(gainDb, 'f', 2) + " dB");
  addRow("Pan", panT);
}

void Inspector::showAudioClip(const QString& name, const QString& source,
                              double startBeats, double durationBeats,
                              float gainDb, double clipOffsetBeats) {
  rebuildGrid();
  title_->setText(name);
  addRow("Source", source);
  addRow("Start", QString::number(startBeats, 'f', 2) + " b");
  addRow("Length", QString::number(durationBeats, 'f', 2) + " b");
  addRow("Trim-in", QString::number(clipOffsetBeats, 'f', 2) + " b");
  addRow("Gain", QString::number(gainDb, 'f', 2) + " dB");
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