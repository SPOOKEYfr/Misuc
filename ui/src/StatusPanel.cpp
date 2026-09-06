#include "StatusPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QDateTime>

#include <misuc/engine/audio/AudioEngine.h>

#include "Theme.h"

namespace misuc::ui {

StatusPanel::StatusPanel(misuc::engine::AudioEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
  auto* lay = new QHBoxLayout(this);
  lay->setContentsMargins(14, 4, 14, 4);
  left_ = new QLabel(QStringLiteral("Idle"), this);
  right_ = new QLabel(this);
  left_->setStyleSheet(
      QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
  right_->setStyleSheet(
      QStringLiteral("color: %1;").arg(theme::kTextFaint.name()));
  lay->addWidget(left_);
  lay->addStretch();
  lay->addWidget(right_);
}

void StatusPanel::refresh() {
  const misuc::engine::BackendStatus st = engine_.backendStatus();
  if (st.running)
    left_->setText(QStringLiteral("● %1 Hz / %2 ch / %3 fpb")
                       .arg(st.negotiatedRate)
                       .arg(st.negotiatedChannels)
                       .arg(st.negotiatedBuffer));
  else
    left_->setText(QStringLiteral("○ Engine idle"));
  right_->setText(QStringLiteral("%1 xruns  •  %2")
                      .arg(engine_.xruns())
                      .arg(QDateTime::currentDateTime().toString(
                          QStringLiteral("hh:mm:ss"))));
}

} // namespace misuc::ui