#include "TransportBar.h"

#include <cmath>

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/transport/Transport.h>

#include "HintSystem.h"
#include "branding/Icons.h"
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
  setAttribute(Qt::WA_Hover, true);
  const misuc::engine::Transport& tr = engine_.transport();
  tempo_ = tr.tempo();
  tsNum_ = tr.timeSigNumerator();
  tsDen_ = tr.timeSigDenominator();
}

void TransportBar::setCountInOn(bool on) { countIn_ = on; }

void TransportBar::setMetronomeVolume(float v) {
  metronomeVol_ = (v < 0.f ? 0.f : (v > 1.f ? 1.f : v));
  update();
}

void TransportBar::setCountInBeats(float b) {
  countInBeats_ = (b < 1.f ? 1.f : (b > 64.f ? 64.f : b));
  update();
}

void TransportBar::setLoopOn(bool on) {
  loopOn_ = on;
  update();
}

void TransportBar::setSnapOn(bool on) {
  snap_ = on;
  update();
}

void TransportBar::setSongMode(bool song) {
  songMode_ = song;
  update();
}

void TransportBar::refresh() {
  const bool p = engine_.isTransportPlaying();
  const bool r = engine_.isRecording();
  const uint64_t ph = engine_.playheadSamples();
  const misuc::engine::Transport& tr = engine_.transport();
  const double t = tr.tempo();
  const int n = tr.timeSigNumerator();
  const int d = tr.timeSigDenominator();
  // Only repaint when something actually changed. This widget aura-paints the
  // entire bar (gradient, buttons, ruler, timecode), so a blanket update() on
  // every 30 Hz poll tick kept repainting it endlessly on an idle machine -
  // the transport mirrored the playlist grid's old flicker + CPU burn.
  if (p == playing_ && r == recording_ && ph == playhead_ && t == tempo_ &&
      n == tsNum_ && d == tsDen_)
    return;
  playing_ = p;
  recording_ = r;
  playhead_ = ph;
  if (t > 0.0) {
    tempo_ = t;
    tsNum_ = n;
    tsDen_ = d;
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
  loopBtn_ = buttonRect(top, 3);
  recordBtn_ = buttonRect(top, 4);
  snapBtn_ = buttonRect(top, 5);
  modeBtn_ = buttonRect(top, 6);

  // Play button.
  {
    QPainterPath path;
    path.addEllipse(playBtn_);
    p.save();
    QColor c = playing_ ? theme::kAccent : theme::kPanelRaised;
    p.setBrush(c);
    p.setPen(QPen(playing_ ? theme::kAccent : theme::kBorderBright, 1.5));
    p.drawPath(path);
    // Glyph: pause (two vertical bars) when playing, triangle (play) when stopped.
    const double cx = playBtn_.center().x();
    const double cy = playBtn_.center().y();
    const double barW = 3.5;
    const double barH = 9;
    const double gap = 3;
    QPainterPath glyph;
    if (playing_) {
      // Pause: two vertical bars
      double leftX = cx - gap - barW;
      double rightX = cx + gap;
      glyph.addRect(leftX, cy - barH / 2, barW, barH);
      glyph.addRect(rightX, cy - barH / 2, barW, barH);
    } else {
      // Play: triangle pointing right
      glyph.moveTo(cx + 5, cy);
      glyph.lineTo(cx - 4, cy + 8);
      glyph.lineTo(cx - 4, cy - 8);
      glyph.closeSubpath();
    }
    p.setPen(Qt::NoPen);
    p.setBrush(playing_ ? theme::kBackground : theme::kText);
    p.drawPath(glyph);
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

  // Loop toggle button.
  {
    QPainterPath path;
    path.addEllipse(loopBtn_);
    p.save();
    p.setBrush(loopOn_ ? theme::kAccentSoft : theme::kPanelRaised);
    p.setPen(QPen(loopOn_ ? theme::kAccent : theme::kBorderBright, 1.5));
    p.drawPath(path);
    p.setPen(loopOn_ ? theme::kAccent : theme::kTextDim);
    p.setFont(theme::uiFont(9, QFont::DemiBold));
    p.drawText(loopBtn_, Qt::AlignCenter, QStringLiteral("LOOP"));
    p.restore();
  }

  // Record button (red when armed, filled dot glyph).
  {
    QPainterPath path;
    path.addEllipse(recordBtn_);
    p.save();
    QColor ring = recording_ ? theme::kDanger : theme::kBorderBright;
    QColor fill = recording_ ? theme::kDangerSoft : theme::kPanelRaised;
    p.setBrush(fill);
    p.setPen(QPen(ring, 1.5));
    p.drawPath(path);
    p.setPen(Qt::NoPen);
    p.setBrush(recording_ ? theme::kDanger : theme::kTextDim);
    const double cx = recordBtn_.center().x(), cy = recordBtn_.center().y();
    p.drawEllipse(QPointF(cx, cy), recording_ ? 7.5 : 5.5, recording_ ? 7.5 : 5.5);
    p.restore();
  }

  // Snap toggle (magnet glyph).
  {
    QPainterPath path;
    path.addEllipse(snapBtn_);
    p.save();
    p.setBrush(snap_ ? theme::kAccentSoft : theme::kPanelRaised);
    p.setPen(QPen(snap_ ? theme::kAccent : theme::kBorderBright, 1.5));
    p.drawPath(path);
    const double cx = snapBtn_.center().x(), cy = snapBtn_.center().y();
    p.setPen(Qt::NoPen);
    p.setBrush(snap_ ? theme::kAccent : theme::kTextDim);
    p.drawArc(QRectF(cx - 5.5, cy - 1, 11, 8), 0, 180 * 16);
    p.drawRect(QRectF(cx - 1.5 + 0.5, cy - 1.5, 1.5, 4));
    p.drawRect(QRectF(cx + 1.5, cy - 1.5, 1.5, 4));
    p.restore();
  }

  // Song/pattern playback mode toggle (arrangement blocks vs single strip).
  {
    QPainterPath path;
    path.addEllipse(modeBtn_);
    p.save();
    p.setBrush(theme::kPanelRaised);
    p.setPen(QPen(theme::kBorderBright, 1.5));
    p.drawPath(path);
    const double cx = modeBtn_.center().x(), cy = modeBtn_.center().y();
    p.setPen(Qt::NoPen);
    p.setBrush(songMode_ ? theme::kAccent : theme::kTextDim);
    const double unit = 3.0;
    if (songMode_) {
      for (int i = 0; i < 3; ++i)
        p.drawRect(QRectF(cx - 6 + i * 5, cy - 2, 4, 4));
      p.drawRect(QRectF(cx - 6, cy + 3, 4, 4));
      p.drawRect(QRectF(cx + 4, cy + 3, 4, 4));
    } else {
      p.drawRect(QRectF(cx - 5, cy - unit, 10, unit));
      p.drawRect(QRectF(cx - 5, cy, 10, unit));
      p.drawRect(QRectF(cx - 5, cy + unit, 10, unit));
    }
    p.restore();
  }

  // Position readout (musical position + elapsed clock) in a two-line field.
  displayRect_ = QRectF(modeBtn_.right() + 24, y, 200, 40);
  p.setBrush(theme::kPanelSunken);
  p.setPen(QPen(theme::kBorder, 1));
  p.drawRoundedRect(displayRect_, 6, 6);
  const QString pos = positionAtFrame(playhead_, tempo_, 48000, tsNum_, tsDen_);
  p.setPen(theme::kText);
  p.setFont(theme::monoFont(14));
  p.drawText(QRectF(displayRect_.left() + 6, displayRect_.top() + 1,
                    displayRect_.width() - 12, 22),
             Qt::AlignVCenter | Qt::AlignLeft, pos);
  // Elapsed clock: HH:MM:SS driven by real engine frames.
  {
    const uint64_t frames = playhead_;
    const uint32_t sr = engine_.sampleRate() > 0 ? engine_.sampleRate() : 48000;
    const qint64 s = static_cast<qint64>(frames / (sr > 0 ? sr : 1));
    const QString clk = QStringLiteral("%1:%2:%3")
                            .arg(s / 3600, 2, 10, QLatin1Char('0'))
                            .arg((s % 3600) / 60, 2, 10, QLatin1Char('0'))
                            .arg(s % 60, 2, 10, QLatin1Char('0'));
    p.setPen(theme::kAccent);
    p.setFont(theme::monoFont(10));
    p.drawText(QRectF(displayRect_.left() + 6, displayRect_.top() + 19,
                      displayRect_.width() - 12, 16),
               Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("CLK %1").arg(clk));
  }

  // Tempo + time-signature readout (live 3-decimal BPM, engine-backed).
  const double tx = displayRect_.right() + 24;
  const double textW = width() - tx - 10;
  p.setPen(theme::kAccent);
  p.setFont(theme::uiFont(8, QFont::DemiBold));
  p.drawText(QRectF(tx, y + 2, textW, 14), Qt::AlignLeft, QStringLiteral("TEMPO · TIME"));
  p.setPen(theme::kText);
  p.setFont(theme::monoFont(13));
  p.drawText(QRectF(tx, y + 15, textW, 26), Qt::AlignLeft,
             QStringLiteral("%1  %2/%3").arg(tempo_, 0, 'f', 3).arg(tsNum_).arg(tsDen_));
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
    if (event->button() == Qt::RightButton) {
      // Cycle the count-in lead-in length (metronome click count).
      const float next =
          countInBeats_ <= 1.0f ? 2.0f : (countInBeats_ <= 2.0f ? 4.0f : 1.0f);
      countInBeats_ = next;
      emit countInBeatsChanged(countInBeats_);
    } else {
      countIn_ = !countIn_;
      emit countInToggled(countIn_);
    }
    update();
  } else if (loopBtn_.contains(pos)) {
    loopOn_ = !loopOn_;
    emit loopToggled(loopOn_);
    update();
  } else if (recordBtn_.contains(pos)) {
    // Open Record Setup dialog instead of directly toggling
    emit recordRequested();
    update();
  } else if (snapBtn_.contains(pos)) {
    snap_ = !snap_;
    emit snapToggled(snap_);
    update();
  } else if (modeBtn_.contains(pos)) {
    songMode_ = !songMode_;
    emit modeToggled(songMode_);
    update();
  }
  QWidget::mousePressEvent(event);
}

void TransportBar::wheelEvent(QWheelEvent* event) {
  // Wheel over the metronome button scrubs the click level (0..1).
  const double stepDeg = event->angleDelta().y() / 8.0;
  if (clickBtn_.contains(QPointF(event->position()))) {
    float v = metronomeVol_ + 0.05f * static_cast<float>(stepDeg / 15.0);
    if (stepDeg > 0.0)
      v = metronomeVol_ + (stepDeg >= 120.0 ? 0.1f : 0.05f);
    else if (stepDeg < 0.0)
      v = metronomeVol_ - (stepDeg <= -120.0 ? 0.1f : 0.05f);
    v = (v < 0.f ? 0.f : (v > 1.f ? 1.f : v));
    if (std::fabs(v - metronomeVol_) > 1e-4f) {
      metronomeVol_ = v;
      emit metronomeVolumeChanged(metronomeVol_);
      update();
    }
    event->accept();
    return;
  }
  QWidget::wheelEvent(event);
}

QString TransportBar::positionText() const {
  return positionAtFrame(playhead_, tempo_,
                         engine_.sampleRate() > 0 ? engine_.sampleRate() : 48000,
                         tsNum_, tsDen_);
}

void TransportBar::attachHints(HintManager* hints) {
  if (!hints)
    return;
  // Guard every capture with a QPointer: the HintManager may invoke this
  // resolver from a queued mouse event even while the transport is being torn
  // down — a raw `this` captured here is exactly what used to crash the app
  // (GP fault in QWidget::mapFromGlobal on a destroyed transport). All reads
  // below go through `self`, so a dead transport yields no hint, never a fault.
  const QPointer<TransportBar> self(this);
  auto mk = [](const QString& name, const QString& desc, const QString& sc,
               icons::IconId ic, std::function<QString()> value = nullptr) {
    Hint h;
    h.name = name;
    h.description = desc;
    h.shortcut = sc;
    h.icon = icons::make(ic, theme::kText, 16);
    h.value = std::move(value);
    return h;
  };
  auto kind = [self]() -> std::optional<int> {
    if (self.isNull())
      return std::nullopt;
    const QPoint pos = self->mapFromGlobal(QCursor::pos());
    const QRectF rf = self->displayRect_;
    if (rf.contains(QPointF(pos)))
      return 7;
    QRectF ref = self->playBtn_;
    if (ref.isNull()) {
      const double y = (self->height() - 40.0) / 2.0;
      ref = QRectF(y, 0, self->width() - 2 * y, 40);
    }
    for (int i = 0; i < 7; ++i) {
      if (self->buttonRect(ref, i).contains(QPointF(pos)))
        return i;
    }
    return std::nullopt;
  };
  auto live = [self](const char* off, const char* on) {
    return [self, off, on]() -> QString {
      if (self.isNull())
        return QString();
      return QLatin1String(self->playing_ ? on : off);
    };
  };
  auto liveBool = [self](const char* off, const char* on,
                         const bool TransportBar::*member) {
    return [self, off, on, member]() -> QString {
      if (self.isNull())
        return QString();
      return QLatin1String((self->*member) ? on : off);
    };
  };
  auto liveText = [self]() -> QString {
    return self.isNull() ? QString() : self->positionText();
  };
  hints->registerCustom(this, [self, mk, kind, live, liveBool,
                               liveText](const QPoint& pos) -> std::optional<Hint> {
    Q_UNUSED(pos);
    if (self.isNull())
      return std::nullopt;
    const std::optional<int> idx = kind();
    if (!idx)
      return std::nullopt;
    switch (*idx) {
      case 0:
        return mk(QStringLiteral("Play"),
                  QLatin1String(self->playing_ ? "Pause playback"
                                               : "Start playback from the playhead"),
                  QStringLiteral("Space"), icons::IconId::Play,
                  live("stopped", "playing"));
      case 1:
        return mk(QStringLiteral("Stop"),
                  QStringLiteral("Return the playhead to the song start"),
                  QStringLiteral("S"), icons::IconId::Stop);
      case 2:
        return mk(QStringLiteral("Metronome"),
                  QStringLiteral("Click and count-in lead; wheel scrubs level, "
                                 "right-click cycles count-in length"),
                  QString(), icons::IconId::Metronome, [self]() -> QString {
                    if (self.isNull())
                      return QString();
                    return QStringLiteral("%1 · %2% · %3-beat")
                        .arg(self->countIn_ ? QLatin1String("on")
                                            : QLatin1String("off"))
                        .arg(qRound(self->metronomeVol_ * 100.0))
                        .arg(qRound(self->countInBeats_));
                  });
      case 3:
        return mk(QStringLiteral("Loop"),
                  QStringLiteral("Loop playback over the active section"),
                  QString(), icons::IconId::Loop,
                  liveBool("off", "on", &TransportBar::loopOn_));
      case 4:
        return mk(QStringLiteral("Record"),
                  QStringLiteral("Arm/disarm input recording at the playhead"),
                  QString(), icons::IconId::Record,
                  liveBool("off", "armed", &TransportBar::recording_));
      case 5:
        return mk(QStringLiteral("Snap"),
                  QStringLiteral("Quantize edits to the grid"),
                  QStringLiteral("N"), icons::IconId::Snap,
                  liveBool("off", "on", &TransportBar::snap_));
      case 6:
        return mk(QStringLiteral("PAT/SONG"),
                  QStringLiteral("Switch between Pattern and Song arrangement"),
                  QString(), icons::IconId::SongMode,
                  liveBool("PAT", "SONG", &TransportBar::songMode_));
      case 7:
        return mk(QStringLiteral("Position"),
                  QStringLiteral("Current musical position (bar.beat.16th.tick)"),
                  QString(), icons::IconId::None, liveText);
      default:
        return std::nullopt;
    }
  });
  hints->setDefaultHint(
      mk(QStringLiteral("Transport"),
         QStringLiteral("Position, tempo and the transport controls"),
         QString(), icons::IconId::None, liveText));
}

} // namespace misuc::ui