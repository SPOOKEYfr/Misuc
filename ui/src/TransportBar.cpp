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
  playing_ = engine_.isTransportPlaying();
  recording_ = engine_.isRecording();
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
    QColor fill = recording_ ? QColor(0x3d, 0x14, 0x14) : theme::kPanelRaised;
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
    countIn_ = !countIn_;
    emit countInToggled(countIn_);
    update();
  } else if (loopBtn_.contains(pos)) {
    loopOn_ = !loopOn_;
    emit loopToggled(loopOn_);
    update();
  } else if (recordBtn_.contains(pos)) {
    recording_ = !recording_;
    emit recordToggled(recording_);
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

QString TransportBar::positionText() const {
  return positionAtFrame(playhead_, tempo_,
                         engine_.sampleRate() > 0 ? engine_.sampleRate() : 48000,
                         tsNum_, tsDen_);
}

void TransportBar::attachHints(HintManager* hints) {
  if (!hints)
    return;
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
  auto kind = [&]() -> std::optional<int> {
    const QPoint pos = mapFromGlobal(QCursor::pos());
    const QRectF rf = displayRect_;
    if (rf.contains(QPointF(pos)))
      return 7;
    QRectF ref = playBtn_;
    if (ref.isNull()) {
      const double y = (height() - 40.0) / 2.0;
      ref = QRectF(y, 0, width() - 2 * y, 40);
    }
    for (int i = 0; i < 7; ++i) {
      if (buttonRect(ref, i).contains(QPointF(pos)))
        return i;
    }
    return std::nullopt;
  };
  hints->registerCustom(this, [&](const QPoint& pos) -> std::optional<Hint> {
    Q_UNUSED(pos);
    const std::optional<int> idx = kind();
    if (!idx)
      return std::nullopt;
    switch (*idx) {
      case 0:
        return mk(QStringLiteral("Transport: Play"),
                  QLatin1String(playing_ ? "Pause playback" : "Start playback from the playhead"),
                  QStringLiteral("Space"), icons::IconId::Play,
                  [&] { return QLatin1String(playing_ ? "playing" : "stopped"); });
      case 1:
        return mk(QStringLiteral("Transport: Stop"),
                  QStringLiteral("Return the playhead to the song start"),
                  QStringLiteral("S"), icons::IconId::Stop);
      case 2:
        return mk(QStringLiteral("Count-In"),
                  QStringLiteral("Play a full bar of click before recording"),
                  QString(), icons::IconId::Metronome,
                  [&] { return countIn_ ? QStringLiteral("on") : QStringLiteral("off"); });
      case 3:
        return mk(QStringLiteral("Loop"),
                  QStringLiteral("Toggle looped playback of the section"),
                  QString(), icons::IconId::Loop,
                  [&] { return loopOn_ ? QStringLiteral("on") : QStringLiteral("off"); });
      case 4:
        return mk(QStringLiteral("Record"),
                  QStringLiteral("Arm/disarm input recording at the playhead"),
                  QString(), icons::IconId::Record,
                  [&] { return recording_ ? QStringLiteral("armed") : QStringLiteral("off"); });
      case 5:
        return mk(QStringLiteral("Snap"),
                  QStringLiteral("Quantize edits to the grid"),
                  QStringLiteral("N"), icons::IconId::Snap,
                  [&] { return snap_ ? QStringLiteral("on") : QStringLiteral("off"); });
      case 6:
        return mk(QStringLiteral("Playback Mode"),
                  QStringLiteral("Switch between Song and Pattern arrangement"),
                  QString(), icons::IconId::SongMode,
                  [&] { return songMode_ ? QStringLiteral("song") : QStringLiteral("pattern"); });
      case 7:
        return mk(QStringLiteral("Transport Position"),
                  QStringLiteral("Current musical position (bar.beat.16th.tick)"),
                  QString(), icons::IconId::None,
                  [&] { return positionText(); });
      default:
        return std::nullopt;
    }
  });
  hints->setDefaultHint(
      mk(QStringLiteral("Transport"),
         QStringLiteral("Position readout, tempo and the transport controls"),
         QString(), icons::IconId::None, [&] { return positionText(); }));
}

} // namespace misuc::ui