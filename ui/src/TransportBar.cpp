#include "TransportBar.h"

#include <cmath>

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/transport/Transport.h>

#include "Theme.h"

namespace misuc::ui {

namespace {
// Render a bar.beat.sixteenth.subtick string for frame N without touching the
// engine's live transport (which the audio thread owns).
QString positionAtFrame(uint64_t frames, double tempo, uint32_t sr, int tsNum,
                        int tsDen) {
  const double beatFrames = 60.0 / tempo * sr * 4.0 / tsDen;
  if (beatFrames <= 0.0)
    return QStringLiteral("1.1.1.1");
  const double posBeats = static_cast<double>(frames) / beatFrames;
  const int bar = static_cast<int>(posBeats / tsNum) + 1;
  const double inBar = posBeats - (bar - 1) * tsNum;
  const int beats = static_cast<int>(inBar) + 1;
  const double rem = inBar - (beats - 1);
  const int sixteenths = static_cast<int>(rem * 4) + 1;
  const double sub = rem * 4 - (sixteenths - 1);
  const int subTicks = static_cast<int>(std::round(sub * 480.0));
  return QStringLiteral("%1.%2.%3.%4")
      .arg(bar)
      .arg(beats)
      .arg(sixteenths)
      .arg(subTicks);
}
} // namespace

TransportBar::TransportBar(misuc::engine::AudioEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
  setMinimumHeight(72);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  const misuc::engine::Transport& tr = engine_.transport();
  tempo_ = tr.tempo();
  tsNum_ = tr.timeSigNumerator();
  tsDen_ = tr.timeSigDenominator();
}

void TransportBar::setCountInOn(bool on) { countIn_ = on; }

void TransportBar::refresh() {
  playing_ = engine_.isTransportPlaying();
  playhead_ = engine_.playheadSamples();
  const misuc::engine::Transport& tr = engine_.transport();
  const double t = tr.tempo();
  if (t > 0.0) {
    tempo_ = t;
    tsNum_ = tr.timeSigNumerator();
    tsDen_ = tr.timeSigDenominator();
  }
  update();
}

QRectF TransportBar::buttonRect(const QRectF& playRect, int index) const {
  const double r = 34.0;
  const double gap = 16.0;
  const double x = playRect.x() + index * (r + gap);
  return QRectF(x, (height() - r) / 2.0, r, r);
}

void TransportBar::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  // Section background.
  p.fillRect(rect(), theme::kPanel);
  p.fillRect(QRect(0, height() - 1, width(), 1), theme::kBorder);

  const double y = (height() - 40.0) / 2.0;
  QRectF top(y, 0, width() - 2 * y, 40);
  playBtn_ = buttonRect(top, 0);
  stopBtn_ = buttonRect(top, 1);
  clickBtn_ = buttonRect(top, 2);

  // Play button.
  {
    QPainterPath path;
    path.addEllipse(playBtn_);
    p.save();
    QColor c = playing_ ? theme::kAccent : theme::kPanelRaised;
    p.setBrush(c);
    p.setPen(QPen(playing_ ? theme::kAccent : theme::kBorderBright, 1.5));
    p.drawPath(path);
    // Triangle glyph.
    QPainterPath tri;
    const double cx = playBtn_.center().x();
    const double cy = playBtn_.center().y();
    tri.moveTo(cx + 6, cy);
    tri.lineTo(cx - 3, cy + 7);
    tri.lineTo(cx - 3, cy - 7);
    tri.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(playing_ ? theme::kBackground : theme::kText);
    p.drawPath(tri);
    p.restore();
  }

  // Stop button (square).
  {
    const QRectF sq = stopBtn_.adjusted(10, 10, -10, -10);
    p.setBrush(theme::kPanelRaised);
    p.setPen(QPen(theme::kBorderBright, 1.5));
    p.drawRect(sq.adjusted(0, 0, -1, -1));
    if (playing_) {
      p.setPen(Qt::NoPen);
      p.setBrush(theme::kDanger);
      p.drawRect(sq.adjusted(6, 6, -6, -6));
    }
  }

  // Count-in toggle (metronome icon).
  {
    QPainterPath path;
    path.addEllipse(clickBtn_);
    p.save();
    p.setBrush(countIn_ ? theme::kAccentSoft : theme::kPanelRaised);
    p.setPen(QPen(countIn_ ? theme::kAccent : theme::kBorderBright, 1.5));
    p.drawPath(path);
    // Simple metronome glyph: a triangle wedge.
    p.setPen(Qt::NoPen);
    p.setBrush(countIn_ ? theme::kAccent : theme::kTextDim);
    const double cx = clickBtn_.center().x(), cy = clickBtn_.center().y();
    QPainterPath wedge;
    wedge.moveTo(cx, cy - 8);
    wedge.lineTo(cx - 6, cy + 7);
    wedge.lineTo(cx - 1, cy + 4);
    wedge.lineTo(cx + 1, cy + 4);
    wedge.lineTo(cx + 6, cy + 7);
    wedge.closeSubpath();
    p.drawPath(wedge);
    p.restore();
  }

  // Position readout.
  displayRect_ = QRectF(clickBtn_.right() + 28, y, 210, 40);
  p.setBrush(theme::kPanelSunken);
  p.setPen(QPen(theme::kBorder, 1));
  p.drawRoundedRect(displayRect_, 6, 6);
  p.setPen(theme::kText);
  p.setFont(theme::monoFont(15));
  const QString pos = positionAtFrame(playhead_, tempo_, 48000, tsNum_, tsDen_);
  p.drawText(displayRect_, Qt::AlignCenter, pos);

  // Tempo readout + label.
  const double tx = displayRect_.right() + 24;
  const double textW = width() - tx - 10;
  p.setPen(theme::kTextDim);
  p.setFont(theme::uiFont(8, QFont::DemiBold));
  p.drawText(QRectF(tx, y + 4, textW, 14), Qt::AlignLeft, QStringLiteral("TEMPO"));
  p.setPen(theme::kText);
  p.setFont(theme::monoFont(14));
  p.drawText(QRectF(tx, y + 18, textW, 22), Qt::AlignLeft,
             QStringLiteral("%1 BPM").arg(tempo_, 0, 'f', 1));

  // Time signature.
  const double sx = tx + 92;
  p.setPen(theme::kTextDim);
  p.setFont(theme::uiFont(8, QFont::DemiBold));
  p.drawText(QRectF(sx, y + 4, textW, 14), Qt::AlignLeft, QStringLiteral("TIME"));
  p.setPen(theme::kText);
  p.setFont(theme::monoFont(14));
  p.drawText(QRectF(sx, y + 18, textW, 22), Qt::AlignLeft,
             QStringLiteral("%1/%2").arg(tsNum_).arg(tsDen_));
}

void TransportBar::mousePressEvent(QMouseEvent* event) {
  const QPointF pos = event->position();
  if (playBtn_.contains(pos)) {
    if (playing_) {
      engine_.pause();
    } else {
      engine_.play(countIn_);
    }
  } else if (stopBtn_.contains(pos)) {
    engine_.stop();
  } else if (clickBtn_.contains(pos)) {
    countIn_ = !countIn_;
    emit countInToggled(countIn_);
    update();
  }
  QWidget::mousePressEvent(event);
}

} // namespace misuc::ui