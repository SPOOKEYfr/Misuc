#include "HeaderBar.h"

#include <cmath>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QTextStream>
#include <QToolButton>
#include <QtGlobal>
#include <unistd.h>

#include <misuc/engine/audio/AudioBackend.h>
#include <misuc/engine/audio/AudioEngine.h>

#include "branding/Logo.h"
#include "Theme.h"
#include "HintSystem.h"
#include "TransportBar.h"
#include "theme/ThemeEngine.h"
#include "tooltip/MisucTooltip.h"

#include <algorithm>

namespace misuc::ui {

namespace {

constexpr int kHeaderHeight = 56;

// Sum utime+stime (fields 14/15 of /proc/<pid>/stat) over every thread of this
// process — a real CPU accounting, not a fake percentage.
uint64_t threadJiffies(const QString& statPath) {
  QFile f(statPath);
  if (!f.open(QIODevice::ReadOnly))
    return 0;
  const QByteArray data = f.readAll();
  const int end = data.lastIndexOf(')');
  if (end < 0)
    return 0;
  const char* p = data.constData() + end + 1;
  int field = 3; // next field after comm is state (field 3)
  uint64_t utime = 0;
  uint64_t stime = 0;
  while (*p && field <= 15) {
    while (*p == ' ' || *p == '\n')
      ++p;
    uint64_t v = 0;
    const char* start = p;
    while (*p && *p != ' ' && *p != '\n')
      ++p;
    const QByteArray token(start, static_cast<int>(p - start));
    if (field == 14)
      utime = token.toULongLong();
    else if (field == 15)
      stime = token.toULongLong();
    ++field;
  }
  return utime + stime;
}

uint64_t processJiffies() {
  uint64_t total = 0;
  const QDir taskDir(QStringLiteral("/proc/self/task"));
  const QFileInfoList infos =
      taskDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QFileInfo& info : infos) {
    bool ok = false;
    info.fileName().toLongLong(&ok);
    if (!ok)
      continue;
    total += threadJiffies(info.absoluteFilePath() + QStringLiteral("/stat"));
  }
  return total;
}

uint64_t processVmRssBytes() {
  QFile f(QStringLiteral("/proc/self/status"));
  if (!f.open(QIODevice::ReadOnly))
    return 0;
  while (!f.atEnd()) {
    const QByteArray line = f.readLine();
    if (line.startsWith("VmRSS:")) {
      const QList<QByteArray> parts = line.trimmed().split(' ');
      if (parts.size() >= 2)
        return parts[1].toULongLong() * 1024ull;
    }
  }
  return 0;
}

QString formatBytes(uint64_t bytes) {
  if (bytes >= 1024ull * 1024 * 1024)
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
  return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
}

} // namespace

// ---------------------------------------------------------------------------
// MiniMeters: two 4px master L/R bars with clip flags, rendered in chrome.
// ---------------------------------------------------------------------------
class MiniMeters : public QWidget {
public:
  explicit MiniMeters(QWidget* parent = nullptr) : QWidget(parent) {
    setFixedSize(26, 20);
    setObjectName(QStringLiteral("headerMeters"));
  }

  void setPeaks(float l, float r, bool clip) {
    l_ = l;
    r_ = r;
    clip_ = clip;
    update();
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    constexpr int kBarW = 6;
    constexpr int kGap = 4;
    const int barH = height() - 2;
    for (int ch = 0; ch < 2; ++ch) {
      const float v = ch == 0 ? l_ : r_;
      const int x = 2 + ch * (kBarW + kGap);
      QColor track = theme::kPanelSunken;
      QColor hot = theme::kPanelSunken;
      for (int i = 0; i < 8; ++i) {
        QRect slot(x, 1 + i * (barH / 8), kBarW, barH / 8 - 1);
        p.fillRect(slot, track);
      }
      const double db = linearToDb(v);
      const double frac = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
      const int lit = static_cast<int>(frac * 8);
      for (int i = 0; i < lit; ++i) {
        QRect slot(x, 1 + (7 - i) * (barH / 8), kBarW, barH / 8 - 1);
        p.fillRect(slot, i >= 6 ? theme::kMeterHot : theme::kMeterNormal);
      }
      if (clip_) {
        QRect slot(x, 1, kBarW, barH / 8 - 1);
        p.fillRect(slot, theme::kMeterClip);
      }
    }
  }

private:
  static double linearToDb(float linear) {
    if (linear <= 1e-6f)
      return -120.0;
    return 20.0 * std::log10(linear);
  }
  float l_ = 0.0f;
  float r_ = 0.0f;
  bool clip_ = false;
};

// ---------------------------------------------------------------------------

HeaderBar::HeaderBar(misuc::engine::AudioEngine& engine, QWidget* parent)
    : QToolBar(QStringLiteral("Header"), parent), engine_(engine) {
  setObjectName(QStringLiteral("headerBar"));
  setMovable(false);
  setFloatable(false);
  setFixedHeight(kHeaderHeight);
  setIconSize(QSize(18, 18));
  clockTicks_ = sysconf(_SC_CLK_TCK);
  if (clockTicks_ <= 0)
    clockTicks_ = 100;

  addWidget(buildLeftCluster());

  QWidget* centre = new QWidget(this);
  auto* centreLay = new QHBoxLayout(centre);
  centreLay->setContentsMargins(0, 0, 0, 0);
  centreLay->addStretch(1);
  buildCenterTransport();
  QWidget* transportHost = new QWidget(centre);
  auto* transportLay = new QHBoxLayout(transportHost);
  transportLay->setContentsMargins(0, 0, 0, 0);
  transportLay->addWidget(transport_);
  transportHost->setFixedWidth(720);
  transportHost->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  centreLay->addWidget(transportHost);
  centreLay->addStretch(1);
  addWidget(centre);
  centre->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  addWidget(buildWorkspaceStrip());
  addWidget(buildRightCluster());

  cpuTimer_.start();
  connect(&ThemeEngine::instance(), &ThemeEngine::themeChanged, this,
          [this] {
            refreshLogo();
            syncStripIcons();
          });
  refreshLogo();
}

QWidget* HeaderBar::buildLeftCluster() {
  auto* host = new QWidget(this);
  host->setObjectName(QStringLiteral("headerBrand"));
  auto* lay = new QHBoxLayout(host);
  lay->setContentsMargins(16, 4, 16, 4);
  lay->setSpacing(10);

  navLogo_ = new QLabel(host);
  navLogo_->setObjectName(QStringLiteral("navLogo"));
  navLogo_->setFixedSize(30, 30);
  navLogo_->setVisible(true);

  wordmark_ = new QLabel(QStringLiteral("MISUC"), host);
  wordmark_->setObjectName(QStringLiteral("wordmark"));
  wordmark_->setStyleSheet(QStringLiteral("color: %1; font-size: 17px; "
                                          "font-weight: 800; letter-spacing: 3px;")
                               .arg(theme::kAccent.name()));

  QFrame* pipe = new QFrame(host);
  pipe->setFrameShape(QFrame::VLine);
  pipe->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextMuted.name()));

  projectName_ = new QLabel(QStringLiteral("Untitled"), host);
  projectName_->setObjectName(QStringLiteral("projectName"));
  projectName_->setStyleSheet(
      QStringLiteral("color: %1; font-size: 12px; font-weight: 600;")
          .arg(theme::kTextSecondary.name()));

  lay->addWidget(navLogo_);
  lay->addWidget(wordmark_);
  lay->addWidget(pipe);
  lay->addWidget(projectName_, 1);
  return host;
}

QWidget* HeaderBar::buildCenterTransport() {
  transport_ = new TransportBar(engine_, this);
  transport_->setMinimumHeight(kHeaderHeight - 8);
  transport_->setMaximumWidth(720);
  return transport_;
}

QWidget* HeaderBar::buildWorkspaceStrip() {
  auto* host = new QWidget(this);
  host->setObjectName(QStringLiteral("workspaceStrip"));
  auto* lay = new QHBoxLayout(host);
  lay->setContentsMargins(8, 4, 8, 4);
  lay->setSpacing(4);

  const struct {
    const char* id;
    icons::IconId icon;
  } kStrip[] = {
      {"playlist", icons::IconId::Playlist},
      {"pianoroll", icons::IconId::PianoRoll},
      {"channelrack", icons::IconId::ChannelRack},
      {"mixer", icons::IconId::Mixer},
      {"browser", icons::IconId::Browser},
      {"inspector", icons::IconId::Inspector},
  };
  for (const auto& [id, icon] : kStrip) {
    QToolButton* b = stripButton(QString::fromLatin1(id), icon);
    wsButtons_.insert(QString::fromLatin1(id), b);
    wsIconIds_.insert(QString::fromLatin1(id), icon);
    lay->addWidget(b);
  }
  return host;
}

QToolButton* HeaderBar::stripButton(const QString& id, icons::IconId icon) {
  auto* b = new QToolButton(this);
  b->setObjectName(QStringLiteral("ws_") + id);
  b->setCheckable(true);
  b->setAutoRaise(true);
  b->setIconSize(QSize(18, 18));
  b->setFixedSize(30, 30);
  b->setCursor(Qt::PointingHandCursor);
  b->setFocusPolicy(Qt::NoFocus);
  connect(b, &QToolButton::clicked, this,
          [this, id] { emit workspaceIconClicked(id); });
  b->setIcon(icons::make(icon, theme::kTextSecondary));
  MisucTooltip::attach(b, QStringLiteral("ws.") + id);
  return b;
}

void HeaderBar::syncStripIcons() {
  for (auto it = wsButtons_.cbegin(); it != wsButtons_.cend(); ++it) {
    const bool active = it.value()->isChecked();
    const icons::IconId id = wsIconIds_.value(it.key());
    it.value()->setIcon(icons::make(
        id, active ? theme::kAccentActive : theme::kTextSecondary));
  }
}

void HeaderBar::setWorkspaceActive(const QString& workspaceId, bool active) {
  QToolButton* b = wsButtons_.value(workspaceId);
  if (!b)
    return;
  b->setChecked(active);
  syncStripIcons();
}

void HeaderBar::setSnapActive(bool on) {
  transport_->setSnapOn(on);
}

void HeaderBar::setSongMode(bool song) {
  transport_->setSongMode(song);
}

QWidget* HeaderBar::buildRightCluster() {
  auto* host = new QWidget(this);
  host->setObjectName(QStringLiteral("headerStatus"));
  auto* lay = new QHBoxLayout(host);
  lay->setContentsMargins(16, 4, 16, 4);
  lay->setSpacing(14);

  const auto mkLabel = [&](QString objectName, const QColor& color,
                           const QString& text) {
    auto* l = new QLabel(text, host);
    l->setObjectName(objectName);
    l->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 600;")
                         .arg(color.name()));
    return l;
  };

  cpuLabel_ = mkLabel("cpuLabel", theme::kTextDim, QStringLiteral("CPU —"));
  ramLabel_ = mkLabel("ramLabel", theme::kTextDim, QStringLiteral("RAM —"));
  srLabel_ = mkLabel("srLabel", theme::kTextDim, QStringLiteral("Hz —"));
  latencyLabel_ = mkLabel("latencyLabel", theme::kTextDim, QStringLiteral("buf —"));
  xrunLabel_ = mkLabel("xrunLabel", theme::kTextFaint, QString());
  stateLabel_ = mkLabel("stateLabel", theme::kMeterClip, QStringLiteral("● idle"));
  meters_ = new MiniMeters(host);

  lay->addWidget(cpuLabel_);
  lay->addWidget(ramLabel_);
  lay->addWidget(srLabel_);
  lay->addWidget(latencyLabel_);
  lay->addWidget(xrunLabel_);
  lay->addWidget(stateLabel_);
  lay->addWidget(meters_);
  return host;
}

void HeaderBar::setProjectName(const QString& name) {
  projectName_->setText(name.isEmpty() ? QStringLiteral("Untitled") : name);
}

void HeaderBar::refresh() {
  refreshCpuRam();

  const misuc::engine::BackendStatus st = engine_.backendStatus();
  if (st.running) {
    srLabel_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; "
                                           "font-weight: 600;")
                                .arg(theme::kTextSecondary.name()));
    srLabel_->setText(QStringLiteral("%1 kHz").arg(st.negotiatedRate / 1000.0, 0,
                                                   'f', 1));
    const misuc::engine::LatencyInfo lat = engine_.latency();
    latencyLabel_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; "
                                                "font-weight: 600;")
                                     .arg(theme::kTextSecondary.name()));
    latencyLabel_->setText(QStringLiteral("%1 fpb · %2 ms out")
                               .arg(st.negotiatedBuffer)
                               .arg(lat.outputMs, 0, 'f', 1));
    xrunLabel_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; "
                                             "font-weight: 700;")
                                  .arg(theme::kMeterClip.name()));
    const uint64_t xruns = engine_.xruns();
    xrunLabel_->setText(xruns > 0 ? QStringLiteral("xruns %1").arg(xruns)
                                  : QString());
    stateLabel_->setText(engine_.isTransportPlaying() ? QStringLiteral("▶ playing")
                                                      : QStringLiteral("⏸ stopped"));
    stateLabel_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; "
                                              "font-weight: 700;")
                                   .arg(engine_.isTransportPlaying()
                                            ? theme::kMeterNormal.name()
                                            : theme::kTextDim.name()));
  } else {
    srLabel_->setText(QStringLiteral("Hz —"));
    latencyLabel_->setText(QStringLiteral("buf —"));
    xrunLabel_->setText(QString());
    stateLabel_->setText(QStringLiteral("● idle"));
    stateLabel_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; "
                                              "font-weight: 600;")
                                   .arg(theme::kMeterClip.name()));
  }

  meters_->setPeaks(engine_.meterPeakL(), engine_.meterPeakR(),
                    engine_.meterClipped());

  transport_->refresh();
}

void HeaderBar::refreshCpuRam() {
  const qint64 ms = cpuTimer_.elapsed();
  uint64_t j = processJiffies();
  if (cpuLastJiffies_ > 0 && ms > 0) {
    const double used = static_cast<double>(j - cpuLastJiffies_) * 1000.0 /
                        (clockTicks_ * ms);
    cpuLabel_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; "
                                            "font-weight: 600;")
                                 .arg(theme::kTextSecondary.name()));
    cpuLabel_->setText(QStringLiteral("CPU %1%").arg(used, 0, 'f', 1));
    const uint64_t rss = processVmRssBytes();
    QColor ramColor = theme::kTextSecondary;
    if (rss > 1024ull * 1024 * 1024)
      ramColor = theme::kMeterHot;
    ramLabel_->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; "
                                            "font-weight: 600;")
                                 .arg(ramColor.name()));
    ramLabel_->setText(QStringLiteral("RAM %1").arg(formatBytes(rss)));
  }
  cpuLastJiffies_ = j;
  cpuTimer_.restart();
}

void HeaderBar::refreshLogo() {
  if (!navLogo_)
    return;
  const QColor accent = theme::kAccent;
  navLogo_->setPixmap(logoPixmap(30, accent, theme::kAccentSoft));
}

void HeaderBar::attachHints(HintManager* hints) {
  if (!hints)
    return;

  struct IdDesc {
    const char* id;
    const char* name;
    const char* desc;
    const char* shortcut;
  };
  static const IdDesc kStripInfo[] = {
      {"playlist", "Playlist", "Arrange audio clips and MIDI regions", "F5"},
      {"pianoroll", "Piano Roll", "Edit MIDI notes for the selected track", "F7"},
      {"channelrack", "Channel Rack", "Program step patterns per channel", "F6"},
      {"mixer", "Mixer", "Gain, pan, mute, solo and meters per track", "M"},
      {"browser", "Browser", "Samples, project audio and instruments", "F8"},
      {"inspector", "Inspector", "Properties of the selected clip or note", "Ctrl+Shift+I"},
  };
  for (const auto& info : kStripInfo) {
    const QString id = QString::fromLatin1(info.id);
    QToolButton* b = wsButtons_.value(id);
    if (!b)
      continue;
    hints->registerTool(
        b, QString::fromUtf8(info.name), QString::fromUtf8(info.desc),
        QString::fromUtf8(info.shortcut), wsIconIds_.value(id),
        [b] { return b->isChecked() ? QStringLiteral("open")
                                    : QStringLiteral("hidden"); });
  }

  if (navLogo_)
    hints->registerTool(navLogo_, QStringLiteral("Misuc"),
                        QStringLiteral("Open the project page"),
                        QStringLiteral("Ctrl+O"), icons::IconId::App);
  if (cpuLabel_)
    hints->registerHint(cpuLabel_, QStringLiteral("App CPU"),
                        QStringLiteral("Avg CPU over the polling window"));
  if (ramLabel_)
    hints->registerHint(ramLabel_, QStringLiteral("App RAM"),
                        QStringLiteral("RSS memory currently resident"));
  if (srLabel_)
    hints->registerHint(srLabel_, QStringLiteral("Audio Engine"),
                        QStringLiteral("Sample rate, channels and buffer size"));
  if (latencyLabel_)
    hints->registerHint(latencyLabel_, QStringLiteral("Latency"),
                        QStringLiteral("Round-trip audio buffer latency"));
  if (xrunLabel_)
    hints->registerHint(xrunLabel_, QStringLiteral("XRuns"),
                        QStringLiteral("Audio underruns / overruns"));
}

} // namespace misuc::ui