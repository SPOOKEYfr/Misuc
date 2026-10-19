#include "Browser.h"

#include <QContextMenuEvent>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QSet>

#include <cmath>
#include <cstring>

#include <sndfile.h>
#include <misuc/engine/audio/WavFile.h>
#include "HintSystem.h"
#include "Theme.h"
#include "elide/ElideLabel.h"
#include "branding/Icons.h"

namespace misuc::ui {

namespace {

// Cap on queued waveform-thumbnail jobs. Beyond this the rows still list and
// audition fine; only the cosmetic per-row waveform is skipped.
constexpr int kWaveformPendingCap = 4096;

QSet<QString> audioExtensions() {
  return {"wav", "flac", "ogg", "mp3", "aif", "aiff"};
}

// Scans `dir` recursively for audio files; honours the per-category cap.
void collectAudio(const QString& dir, int cap, QStringList& out) {
  QDirIterator it(dir, QDir::Files | QDir::Readable,
                  QDirIterator::Subdirectories);
  while (it.hasNext() && out.size() < cap) {
    const QString f = it.next();
    if (audioExtensions().contains(QFileInfo(f).suffix().toLower()))
      out << f;
  }
}

// The guaranteed "Sounds" library: the app-data Sounds folder (created and
// seeded on first run) plus the user's home Sounds/Samples folders. Only
// existing directories are returned so a fresh machine still reaches the
// app-backed library.
QStringList soundsLibraryRoots() {
  QStringList roots;
  const QString app =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (!app.isEmpty()) {
    const QDir lib(app + QStringLiteral("/Sounds"));
    if (lib.exists())
      roots << lib.absolutePath();
  }
  QDir home = QDir::home();
  for (const QString& rel : {QStringLiteral("Sounds"),
                             QStringLiteral("Samples")}) {
    const QDir d(home.absoluteFilePath(rel));
    if (d.exists())
      roots << d.absolutePath();
  }
  return roots;
}

// Decodes `path` to a compact amplitude envelope without ever holding the
// whole file in memory: the source is streamed in blocks through libsndfile
// and each of `bins` thumbnails captures the peak |sample| of its window,
// normalized against the loudest window in the file. Mono input downsamples
// to a single envelope; stereo takes the max across channels.
bool computeEnvelope(const QString& path, int bins, QVector<float>& out) {
  if (bins <= 0)
    return false;
  out.fill(0.0f, bins);
  SF_INFO info;
  std::memset(&info, 0, sizeof(info));
  SNDFILE* sf = sf_open(QFile::encodeName(path).constData(), SFM_READ, &info);
  if (!sf)
    return false;
  if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) {
    sf_close(sf);
    return false;
  }
  int64_t framesRead = 0;
  const auto total = static_cast<int64_t>(info.frames);
  std::vector<float> chunk(static_cast<size_t>(info.channels) * 65536);
  float globalMax = 0.0001f; // avoid divide-by-zero for silent files
  while (framesRead < total) {
    const auto want = static_cast<sf_count_t>(
        std::min<int64_t>(65536, total - framesRead));
    const sf_count_t got = sf_readf_float(sf, chunk.data(), want);
    if (got <= 0)
      break;
    for (int64_t i = 0; i < got; ++i) {
      float peak = 0.f;
      for (int c = 0; c < info.channels; ++c) {
        const float v = chunk[static_cast<size_t>(i * info.channels + c)];
        peak = std::max(peak, std::fabs(v));
      }
      if (peak > globalMax)
        globalMax = peak;
      const int64_t fIdx = framesRead + i;
      const int64_t bin =
          (fIdx * static_cast<int64_t>(bins)) / std::max<int64_t>(1, total);
      if (bin >= 0 && bin < bins && peak > out[static_cast<int>(bin)])
        out[static_cast<int>(bin)] = peak;
    }
    framesRead += got;
  }
  sf_close(sf);
  if (globalMax > 0.0f)
    for (int i = 0; i < bins; ++i)
      out[i] /= globalMax;
  return true;
}

// Async waveform thumbnail task: streams the file on a worker thread and posts
// the envelope back to the Browser on the GUI thread. Cheap enough that the
// thumbnail for a whole folder of small samples stays responsive.
class WaveformTask : public QRunnable {
public:
  WaveformTask(QString path, int bins, QPointer<Browser> target)
      : path_(std::move(path)), bins_(bins), target_(std::move(target)) {}

  void run() override {
    QVector<float> binsv;
    if (!computeEnvelope(path_, bins_, binsv))
      return;
    if (!target_)
      return;
    const QPointer<Browser> target = target_;
    const QString p = path_;
    const QVector<float> b = binsv;
    QMetaObject::invokeMethod(
        target,
        [target, p, b] {
          if (target)
            target->onWaveformReady(p, b);
        },
        Qt::QueuedConnection);
  }

private:
  QString path_;
  int bins_;
  QPointer<Browser> target_;
};

// Paints the cached 96-bin amplitude envelope for each file's shape column.
// Rows without cached data keep a flat baseline so the list never flashes.
class WaveformDelegate : public QStyledItemDelegate {
public:
  QHash<QString, QVector<float>>* cache = nullptr;

  void paint(QPainter* p, const QStyleOptionViewItem& opt,
             const QModelIndex& idx) const override {
    QStyledItemDelegate::paint(p, opt, idx);
    if (!cache)
      return;
    const QString path = idx.data(Qt::UserRole).toString();
    if (path.isEmpty())
      return;
    const auto it = cache->constFind(path);
    if (it == cache->constEnd() || it.value().isEmpty())
      return;
    const QRect r = opt.rect.adjusted(2, 2, -2, -2);
    if (r.width() < 10 || r.height() < 4)
      return;
    p->save();
    QColor col = theme::kTextDim;
    if (opt.state.testFlag(QStyle::State_Selected))
      col = theme::kAccent;
    p->setPen(QPen(col, 1.0));
    const float mid = r.top() + r.height() * 0.5f;
    const int n = qMin<int>(it.value().size(), r.width() / 2);
    const float dx = r.width() / float(qMax(1, n));
    float prevX = r.left();
    float prevY = mid;
    for (int i = 0; i < n; ++i) {
      const float x = r.left() + float(i) * dx;
      const float amp = qBound(0.0f, it.value()[i], 1.0f);
      const float y = mid - amp * r.height() * 0.5f;
      p->drawLine(QPointF(prevX, prevY), QPointF(x, y));
      prevX = x;
      prevY = y;
    }
    p->drawLine(QPointF(prevX, prevY), QPointF(r.right(), mid));
    p->restore();
  }
};

// Folders the browser treats as a "SOUNDS" library rather than the ambient
// home-directory audio: the user's sample packs and drum kits on the shared
// Windows partition. Only existing directories are returned, so a machine
// without that mount simply starts empty.
QStringList discoveredDefaultPacks() {
  QStringList packs;
  const QString user = qEnvironmentVariable("USER");
  if (user.isEmpty())
    return packs;
  const QString win = QStringLiteral("/media/") + user +
                      QStringLiteral("/Windows/Downloads");
  const QStringList direct = {QStringLiteral("/lofi"),
                              QStringLiteral("/lofi/Samples"),
                              QStringLiteral("/LoFi Drum Kit 2022"),
                              QStringLiteral("/Phonk Drum Kit")};
  for (const QString& rel : direct) {
    const QString dir = win + rel;
    if (QDir(dir).exists())
      packs << QDir(dir).absolutePath();
  }
  // Drum kits named *KIT* anywhere under the FL project dump.
  const QDir projects(win + QStringLiteral("/Projects"));
  if (projects.exists()) {
    for (const QString& entry : projects.entryList(
             QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
      if (entry.contains(QStringLiteral("KIT"), Qt::CaseInsensitive))
        packs << projects.absoluteFilePath(entry);
    }
  }
  return packs;
}

} // namespace

Browser::Browser(QWidget* parent) : QWidget(parent) {
  // No hard minimum: the browser is a real resizable panel (dock min width in
  // WorkspaceManager is the floor). Left/right panes collapse at the edge so
  // docked columns can be genuinely narrowed by the splitter.
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* split = new QSplitter(Qt::Horizontal, this);
  split->setChildrenCollapsible(true);
  outer->addWidget(split);

  cats_ = new QTreeWidget(split);
  cats_->setObjectName(QStringLiteral("sampleCats"));
  cats_->setHeaderHidden(true);
  cats_->setFocusPolicy(Qt::NoFocus);
  cats_->setStyleSheet(QString("background: %1; border-right: 1px solid %2;")
      .arg(theme::kSurface.name(QColor::HexArgb),
           theme::kChromeBorder.name(QColor::HexArgb)));
  split->addWidget(cats_);

  auto* rightCol = new QWidget(split);
  auto* rcLay = new QVBoxLayout(rightCol);
  rcLay->setContentsMargins(6, 6, 6, 6);
  rcLay->setSpacing(6);

  auto* searchHeader = new QHBoxLayout;
  auto* search = new QLineEdit(rightCol);
  searchBox_ = search;
  search->setPlaceholderText("Search samples...");
  search->setClearButtonEnabled(true);
  search->setMinimumWidth(48);
  auto* searchIcon = new QLabel(rightCol);
  searchIcon->setPixmap(icons::make(icons::IconId::Search, theme::kTextDim, 16).pixmap(16, 16));
  searchHeader->addWidget(searchIcon);
  searchHeader->addWidget(search, 1);
  rcLay->addLayout(searchHeader);

  files_ = new QTreeWidget(rightCol);
  files_->setObjectName(QStringLiteral("sampleFiles"));
  files_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Tag"),
                           QStringLiteral("Shape")});
  files_->setRootIsDecorated(false);
  files_->setFocusPolicy(Qt::NoFocus);
  files_->setDragDropMode(QAbstractItemView::DragOnly);
  files_->header()->setStretchLastSection(false);
  files_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  files_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
  files_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  files_->header()->resizeSection(1, 72);
  files_->setStyleSheet(QString("background: %1;").arg(theme::kSurface.name(QColor::HexArgb)));
  auto* wave = new WaveformDelegate;
  wave->cache = &waveformCache_;
  files_->setItemDelegateForColumn(2, wave);
  (void)wave;
  files_->viewport()->installEventFilter(this);
  files_->installEventFilter(this);
  files_->setContextMenuPolicy(Qt::CustomContextMenu);
  rcLay->addWidget(files_, 1);

  // bottom preview strip (matte-glass chrome)
  auto* preview = new QWidget(rightCol);
  preview->setStyleSheet(QString("background: %1; border-top: 1px solid %2;")
      .arg(theme::kChrome.name(QColor::HexArgb),
           theme::kChromeBorder.name(QColor::HexArgb)));
  previewHost_ = new QVBoxLayout(preview);
  previewHost_->setContentsMargins(8, 6, 8, 6);
  previewHost_->setSpacing(4);
  previewTitle_ = new ElideLabel(preview);
  previewTitle_->setObjectName(QStringLiteral("previewTitle"));
  previewTitle_->setText(QStringLiteral("Select an audio file to preview"));
  previewTitle_->setStyleSheet("color: " + theme::kTextFaint.name());
  previewTitle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  previewMeta_ = new ElideLabel(preview);
  previewMeta_->setObjectName(QStringLiteral("previewMeta"));
  previewMeta_->setStyleSheet("color: " + theme::kTextDim.name());
  previewMeta_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  previewHost_->addWidget(previewTitle_);
  previewHost_->addWidget(previewMeta_);
  previewProgress_ = new QProgressBar(preview);
  previewProgress_->setRange(0, 100);
  previewProgress_->setValue(0);
  previewProgress_->setTextVisible(false);
  previewProgress_->setFixedHeight(4);
  previewProgress_->setStyleSheet(
      QString("QProgressBar { background: %1; border: none; }"
              "QProgressBar::chunk { background: %2; }")
          .arg(theme::kChromeBorder.name(QColor::HexArgb),
               theme::kAccent.name(QColor::HexArgb)));
  previewHost_->addWidget(previewProgress_);

  auto* btnRow = new QHBoxLayout;
  btnRow->setSpacing(2);
  previewBtn_ = new QToolButton(preview);
  previewBtn_->setObjectName(QStringLiteral("previewBtn"));
  previewBtn_->setIcon(icons::make(icons::IconId::Preview, theme::kText, 16));
  previewBtn_->setIconSize(QSize(16, 16));
  previewBtn_->setAutoRaise(true);
  previewBtn_->setFixedSize(24, 24);
  previewBtn_->setCursor(Qt::PointingHandCursor);
  previewBtn_->setFocusPolicy(Qt::NoFocus);
  previewBtn_->setToolTip(QStringLiteral("Play / pause preview"));
  previewStopBtn_ = new QToolButton(preview);
  previewStopBtn_->setObjectName(QStringLiteral("previewStopBtn"));
  previewStopBtn_->setIcon(icons::make(icons::IconId::Stop, theme::kText, 16));
  previewStopBtn_->setIconSize(QSize(16, 16));
  previewStopBtn_->setAutoRaise(true);
  previewStopBtn_->setFixedSize(24, 24);
  previewStopBtn_->setCursor(Qt::PointingHandCursor);
  previewStopBtn_->setFocusPolicy(Qt::NoFocus);
  previewStopBtn_->setToolTip(QStringLiteral("Stop preview"));
  favBtn_ = new QToolButton(preview);
  favBtn_->setObjectName(QStringLiteral("favBtn"));
  favBtn_->setIconSize(QSize(16, 16));
  favBtn_->setAutoRaise(true);
  favBtn_->setFixedSize(24, 24);
  favBtn_->setCursor(Qt::PointingHandCursor);
  favBtn_->setFocusPolicy(Qt::NoFocus);
  favBtn_->setToolTip(QStringLiteral("Favourite sample (persisted)"));
  insertBtn_ = new QToolButton(preview);
  insertBtn_->setIcon(icons::make(icons::IconId::Add, theme::kAccent, 16));
  insertBtn_->setIconSize(QSize(16, 16));
  insertBtn_->setAutoRaise(true);
  insertBtn_->setFixedSize(24, 24);
  insertBtn_->setCursor(Qt::PointingHandCursor);
  insertBtn_->setFocusPolicy(Qt::NoFocus);
  scanBtn_ = new QToolButton(preview);
  scanBtn_->setObjectName(QStringLiteral("scanPackBtn"));
  scanBtn_->setIcon(icons::make(icons::IconId::Folder, theme::kTextDim, 16));
  scanBtn_->setIconSize(QSize(16, 16));
  scanBtn_->setAutoRaise(true);
  scanBtn_->setFixedSize(24, 24);
  scanBtn_->setCursor(Qt::PointingHandCursor);
  scanBtn_->setFocusPolicy(Qt::NoFocus);
  scanBtn_->setToolTip(QStringLiteral("Scan Sample Pack Folder..."));
  refreshBtn_ = new QToolButton(preview);
  refreshBtn_->setObjectName(QStringLiteral("refreshSoundsBtn"));
  refreshBtn_->setIcon(icons::make(icons::IconId::Refresh, theme::kTextDim, 16));
  refreshBtn_->setIconSize(QSize(16, 16));
  refreshBtn_->setAutoRaise(true);
  refreshBtn_->setFixedSize(24, 24);
  refreshBtn_->setCursor(Qt::PointingHandCursor);
  refreshBtn_->setFocusPolicy(Qt::NoFocus);
  refreshBtn_->setToolTip(QStringLiteral("Refresh Sounds Library (rescan)"));
  btnRow->addWidget(previewBtn_);
  btnRow->addWidget(previewStopBtn_);
  btnRow->addWidget(favBtn_);
  btnRow->addWidget(insertBtn_);
  btnRow->addWidget(scanBtn_);
  btnRow->addWidget(refreshBtn_);
  btnRow->addStretch(1);
  previewHost_->addLayout(btnRow);
  auto* volRow = new QHBoxLayout;
  volRow->setSpacing(4);
  auto* volLab = new QLabel(QStringLiteral("Vol"), preview);
  volLab->setStyleSheet("color: " + theme::kTextFaint.name());
  volRow->addWidget(volLab);
  previewVolumeSlider_ = new QSlider(Qt::Horizontal, preview);
  previewVolumeSlider_->setObjectName(QStringLiteral("previewVolume"));
  previewVolumeSlider_->setRange(0, 100);
  { // Persisted preview volume (QSettings "browser" group).
    QSettings sv;
    sv.beginGroup(QStringLiteral("browser"));
    previewVolumeSlider_->setValue(
        qBound(0, sv.value(QStringLiteral("previewVolume"), 50).toInt(), 100));
  }
  previewVolumeSlider_->setFixedWidth(72);
  previewVolumeSlider_->setFixedHeight(18);
  previewVolumeSlider_->setCursor(Qt::PointingHandCursor);
  previewVolumeSlider_->setFocusPolicy(Qt::NoFocus);
  previewVolumeSlider_->setToolTip(QStringLiteral("Preview volume"));
  volRow->addWidget(previewVolumeSlider_);
  volRow->addStretch(1);
  previewHost_->addLayout(volRow);
  rcLay->addWidget(preview);

  split->addWidget(rightCol);
  split->setStretchFactor(0, 1);
  split->setStretchFactor(1, 3);
  split->setSizes({150, 360});

  // categories
  auto* allC  = new QTreeWidgetItem(cats_, {QStringLiteral("All Samples")});
  soundsParent_ = new QTreeWidgetItem(cats_, {QStringLiteral("Sounds")});
  auto* curC  = new QTreeWidgetItem(cats_, {QStringLiteral("Current Project")});
  auto* favC  = new QTreeWidgetItem(cats_, {QStringLiteral("Favourites")});
  auto* recC  = new QTreeWidgetItem(cats_, {QStringLiteral("Recent")});
  packsParent_ = new QTreeWidgetItem(cats_, {QStringLiteral("Packs")});
  allC->setIcon(0, icons::make(icons::IconId::Audio, theme::kTextDim, 16));
  soundsParent_->setIcon(0, icons::make(icons::IconId::Samples, theme::kTextDim, 16));
  soundsParent_->setToolTip(0, QStringLiteral("The default Misuc sounds library"));
  curC->setIcon(0, icons::make(icons::IconId::CurrentProject, theme::kTextDim, 16));
  favC->setIcon(0, icons::make(icons::IconId::Favorites, theme::kTextDim, 16));
  recC->setIcon(0, icons::make(icons::IconId::Recent, theme::kTextDim, 16));
  packsParent_->setIcon(0, icons::make(icons::IconId::Samples, theme::kTextDim, 16));

  // A fresh profile indexes the user's real packs once; after that the pack
  // list is whatever the user scanned/removed (persisted).
  {
    const QStringList existing = packDirs();
    if (existing.isEmpty()) {
      QStringList seeded = discoveredDefaultPacks();
      if (!seeded.isEmpty())
        savePackDirs(seeded);
    }
    rebuildPackNodes();
  }
  // The Sounds library is filesystem-backed: guaranteed folders, seeded from
  // the project's real audio, and a recursive folder tree from every root.
  rebuildSoundNodes();

  cats_->expandAll();
  // Start on "Current Project" so app launch never triggers the full sample
  // scan (which walks every indexed pack, e.g. sample packs on a large shared
  // NTFS partition, and opens every listed file for its waveform thumbnail).
  // The user picks a scan-heavy category (Sounds / All Samples / Packs) when
  // they actually want to browse.
  cats_->setCurrentItem(curC);

  previewTimer_ = new QTimer(this);
  previewTimer_->setInterval(60);
  previewTimer_->setTimerType(Qt::PreciseTimer);
  connect(previewTimer_, &QTimer::timeout, this, &Browser::onPreviewTick);

  // Waveform thumbnails never get to own all cores: two workers keep pack
  // browsing responsive while leaving the audio thread headroom.
  waveformPool_ = new QThreadPool(this);
  waveformPool_->setMaxThreadCount(2);
  waveformPool_->setExpiryTimeout(30000);

  connect(search, &QLineEdit::textChanged, this,
          &Browser::onSearchTextChanged);

  connect(cats_, &QTreeWidget::currentItemChanged, this,
          [this] { onCategoryActivated(); });
  connect(files_, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem* cur) { selectFileItem(cur); });
  connect(files_, &QTreeWidget::itemDoubleClicked, this,
          [this] { onFileActivated(); });
  connect(previewBtn_, &QToolButton::clicked, this, [this] {
    // Idle → play; playing → pause; paused → resume (the engine holds the
    // position, so a resume continues exactly where the audio left off).
    if (previewActive_ && !previewPaused_) {
      previewPaused_ = true;
      previewElapsedMs_ += previewClock_.elapsed();
      Q_EMIT previewPauseChanged(true);
      if (previewTimer_)
        previewTimer_->stop();
      updatePreviewChrome();
      return;
    }
    if (previewActive_ && previewPaused_) {
      previewPaused_ = false;
      resumePreview();
      return;
    }
    const QString p = currentFilePath();
    if (p.isEmpty())
      return;
    startPreview(p);
  });
  connect(previewStopBtn_, &QToolButton::clicked, this, [this] {
    stopPreview(true);
  });
  connect(previewVolumeSlider_, &QSlider::valueChanged, this,
          [this](int v) {
            QSettings sv;
            sv.beginGroup(QStringLiteral("browser"));
            sv.setValue(QStringLiteral("previewVolume"), v);
            sv.endGroup();
            Q_EMIT previewVolumeChanged(previewVolume());
          });
  connect(favBtn_, &QToolButton::clicked, this, [this] { toggleFavourite(); });
  connect(insertBtn_, &QToolButton::clicked, this,
          [this] {
            const QString p = currentFilePath();
            if (!p.isEmpty()) {
              emit insertSample(p);
              touchRecent(p);
            }
          });
  connect(scanBtn_, &QToolButton::clicked, this, [this] {
    selectScanFolder();
  });
  connect(refreshBtn_, &QToolButton::clicked, this, [this] {
    refreshSoundsLibrary();
  });
  connect(files_, &QTreeWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) {
            const QString p = currentFilePath();
            QMenu menu(this);
            QAction* previewAct = menu.addAction(QStringLiteral("Preview"));
            QAction* insertAct = menu.addAction(QStringLiteral("Insert to Playlist"));
            QAction* favAct =
                menu.addAction(p.isEmpty() || !isFavourite(p)
                                   ? QStringLiteral("Add to Favourites")
                                   : QStringLiteral("Remove from Favourites"));
            menu.addSeparator();
            QAction* refreshAct = menu.addAction(QStringLiteral("Refresh Sounds Library"));
            QAction* chosen = menu.exec(files_->viewport()->mapToGlobal(pos));
            if (!chosen)
              return;
            if (chosen == previewAct)
              startPreview(p);
            else if (chosen == insertAct) {
              if (!p.isEmpty()) {
                emit insertSample(p);
                touchRecent(p);
              }
            } else if (chosen == favAct)
              toggleFavourite();
            else if (chosen == refreshAct)
              refreshSoundsLibrary();
          });
  connect(cats_, &QTreeWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) {
            QMenu menu(this);
            QAction* scanAct =
                menu.addAction(QStringLiteral("Scan Sample Pack Folder..."));
            QAction* refreshAct =
                menu.addAction(QStringLiteral("Refresh Sounds Library"));
            QAction* chosen = menu.exec(cats_->viewport()->mapToGlobal(pos));
            if (chosen == scanAct)
              selectScanFolder();
            else if (chosen == refreshAct)
              refreshSoundsLibrary();
          });
  onCategoryActivated();
}

Browser::~Browser() { stopPreview(false); }

bool Browser::eventFilter(QObject* watched, QEvent* event) {
  if ((watched == files_ || watched == files_->viewport()) &&
      event->type() == QEvent::MouseButtonPress) {
    auto* me = static_cast<QMouseEvent*>(event);
    if (me->button() == Qt::LeftButton) {
      // Single-click on an audio row = immediate audition. Clicking another
      // sound stops the previous preview and starts the new one (the engine
      // preview slot handles that handover); the same row re-click toggles
      // nothing extra (it is already the selection).
      const QModelIndex idx = files_->indexAt(me->pos());
      if (idx.isValid()) {
        const QString p = files_->itemFromIndex(idx)->data(0, Qt::UserRole).toString();
        if (!p.isEmpty())
          startPreview(p);
      }
    }
  }
  return QWidget::eventFilter(watched, event);
}

bool Browser::indexPackFolder(const QString& dir) {
  QFileInfo info(dir);
  if (!info.isDir())
    return false;
  const QString abs = info.absoluteFilePath();
  QStringList dirs = packDirs();
  if (!dirs.contains(abs)) {
    dirs << abs;
    savePackDirs(dirs);
  }
  rebuildPackNodes();
  onCategoryActivated(); // either a refreshed pack listing or the same view
  return true;
}

void Browser::attachHints(HintManager* hints) {
  if (!hints)
    return;
  hints->registerTool(searchBox_, QStringLiteral("Search Samples"),
                      QStringLiteral("Filter the sample list by name"), QString(), icons::IconId::Search);
  hints->registerTool(previewBtn_, QStringLiteral("Preview Sample"),
                      QStringLiteral("Audition the selected audio file"), QString(), icons::IconId::Preview,
                      [this] {
                        return hasPreviewSelection() ? currentPreviewName()
                                                     : QStringLiteral("no selection");
                      });
  hints->registerTool(previewStopBtn_, QStringLiteral("Stop Preview"),
                      QStringLiteral("Halt the current audition"), QString(),
                      icons::IconId::Stop);
  hints->registerTool(favBtn_, QStringLiteral("Favourite Sample"),
                      QStringLiteral("Pin the selected sample to the "
                                     "Favourites list (persisted)"), QString(),
                      icons::IconId::Favorites);
  hints->registerTool(insertBtn_, QStringLiteral("Insert Sample"),
                      QStringLiteral("Place the selected audio as a new clip in "
                                     "the playlist"), QString(), icons::IconId::Add,
                      [this] { return QString(); });
  hints->registerTool(scanBtn_, QStringLiteral("Scan Sample Pack"),
                      QStringLiteral("Index a folder of samples as a pack"),
                      QString(), icons::IconId::Folder);
  hints->registerTool(refreshBtn_, QStringLiteral("Refresh Sounds"),
                      QStringLiteral("Rescan the Sounds library folders and "
                                     "re-index the sample list"), QString(),
                      icons::IconId::Refresh);
  hints->registerTool(previewVolumeSlider_, QStringLiteral("Preview Volume"),
                      QStringLiteral("Level of the sample audition"),
                      QString(), icons::IconId::None,
                      [this] {
                        return QStringLiteral("%1%").arg(previewVolumeSlider_->value());
                      });
}

void Browser::populateSamples() { onCategoryActivated(); }

void Browser::onCategoryActivated() {
  auto* cur = cats_->currentItem();
  if (!cur) return;
  files_->clear();
  const QString cat = cur->text(0);
  const QString packDir = cur->parent() == packsParent_
                              ? cur->data(0, Qt::UserRole).toString()
                              : QString();
  const bool isSoundsTop = cur == soundsParent_;
  const bool isSoundNode = cur->parent() == soundsParent_ ||
                           (cur->parent() && cur->parent()->parent() == soundsParent_);

  QStringList baseDirs;
  QString musicDir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
  if (!musicDir.isEmpty()) baseDirs << musicDir;
  QDir home = QDir::home();
  baseDirs << home.absoluteFilePath("Music")
           << home.absoluteFilePath("Samples")
           << home.absoluteFilePath("Sounds");
  baseDirs += packDirs();
  const QStringList soundRoots = soundsLibraryRoots();
  QString soundDir;
  if (isSoundNode && !isSoundsTop) {
    // Any node inside the Sounds scope (Library, Library → Instruments, or a
    // named home folder) resolves to a real directory on disk; collecting it
    // recursively lists exactly the seeded layout.
    const QString d = cur->data(0, Qt::UserRole).toString();
    if (QFileInfo(d).isDir())
      soundDir = d;
  }

  QStringList names;
  if (cat == "Current Project")
    names = sessionSources_;
  else if (cat == "Favourites")
    names = favourites();
  else if (cat == "Recent")
    names = recent();
  else if (isSoundsTop)
    for (const QString& d : soundRoots)
      collectAudio(d, 4000, names);
  else if (!soundDir.isEmpty())
    collectAudio(soundDir, 4000, names); // a single sound library folder
  else if (!packDir.isEmpty())
    collectAudio(packDir, 600, names); // a single pack
  else if (cat == "Packs")
    for (const QString& d : packDirs())
      collectAudio(d, 4000, names); // every indexed pack
  else
    for (const QString& d : baseDirs)
      collectAudio(d, 4000, names); // All Samples

  for (auto& f : names) {
    const QFileInfo fi(f);
    if (!fi.exists() || !fi.isReadable())
      continue;
    auto* it = new QTreeWidgetItem(files_);
    it->setText(0, fi.fileName());
    it->setText(1, QString("%1 KB").arg(qMax(1, int(fi.size() / 1024))));
    it->setIcon(0, icons::make(icons::IconId::Audio, theme::kTextDim, 14));
    it->setData(0, Qt::UserRole, f);
    it->setData(2, Qt::UserRole, f);
    it->setToolTip(0, f); // long names stay reachable via tooltip (Comp07)
    enqueueWaveform(f);
  }
  previewTitle_->setText(QString::number(files_->topLevelItemCount()) + " audio files");
  previewMeta_->setText(QString());
  refreshFavIcon();
}

void Browser::onSearchTextChanged(const QString& text) {
  if (text.isEmpty()) {
    onCategoryActivated();
    return;
  }
  // Filename search across the whole sample index (default locations + the
  // Sounds library + every indexed pack + favourites/recent), not just the
  // visible category.
  files_->clear();
  QStringList names;
  QString musicDir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
  if (!musicDir.isEmpty()) names << musicDir;
  QDir home = QDir::home();
  names << home.absoluteFilePath("Music")
        << home.absoluteFilePath("Samples")
        << home.absoluteFilePath("Sounds");
  names += soundsLibraryRoots();
  names += packDirs();
  names += favourites();
  names += recent();
  QStringList hits;
  for (const QString& d : names) {
    const QFileInfo di(d);
    if (!di.isDir())
      continue;
    QDirIterator it(d, QDir::Files | QDir::Readable,
                    QDirIterator::Subdirectories);
    while (it.hasNext() && hits.size() < 4000) {
      const QString f = it.next();
      if (audioExtensions().contains(QFileInfo(f).suffix().toLower()) &&
          QFileInfo(f).fileName().contains(text, Qt::CaseInsensitive))
        hits << f;
    }
  }
  for (const QString& f : hits) {
    const QFileInfo fi(f);
    auto* it = new QTreeWidgetItem(files_);
    it->setText(0, fi.fileName());
    it->setText(1, QString("%1 KB").arg(qMax(1, int(fi.size() / 1024))));
    it->setIcon(0, icons::make(icons::IconId::Audio, theme::kTextDim, 14));
    it->setData(0, Qt::UserRole, f);
    it->setData(2, Qt::UserRole, f);
    it->setToolTip(0, f); // long names stay reachable via tooltip (Comp07)
    enqueueWaveform(f);
  }
  previewTitle_->setText(QString::number(files_->topLevelItemCount()) +
                         " matches");
  previewMeta_->setText(QString());
  refreshFavIcon();
}

void Browser::selectFileItem(QTreeWidgetItem* cur) {
  if (!cur) return;
  const QString p = cur->data(0, Qt::UserRole).toString();
  if (!p.isEmpty()) {
    buildPreview(p);
    refreshFavIcon();
  }
}

void Browser::onFileActivated() {
  const QString p = currentFilePath();
  if (!p.isEmpty()) {
    startPreview(p);
    emit insertSample(p);
    touchRecent(p);
  }
}

QString Browser::currentFilePath() const {
  if (files_->currentItem())
    return files_->currentItem()->data(0, Qt::UserRole).toString();
  return QString();
}

void Browser::startPreview(const QString& path) {
  if (path.isEmpty() || !QFileInfo(path).exists())
    return;
  buildPreview(path);
  // A new audition always announces the engine handover; MainWindow stops the
  // previous preview before starting this one (no duplicate audio playback).
  Q_EMIT previewFile(path);
  touchRecent(path);
  refreshFavIcon();
  previewActive_ = true;
  previewPaused_ = false;
  previewElapsedMs_ = 0;
  previewClock_.restart();
  if (previewProgress_)
    previewProgress_->setValue(0);
  if (previewTimer_)
    previewTimer_->start();
  updatePreviewChrome();
}

void Browser::stopPreview(bool notifyEngine) {
  if (notifyEngine)
    Q_EMIT previewStopRequested();
  previewActive_ = false;
  previewPaused_ = false;
  previewElapsedMs_ = 0;
  if (previewTimer_)
    previewTimer_->stop();
  if (previewProgress_)
    previewProgress_->setValue(0);
  updatePreviewChrome();
}

void Browser::resumePreview() {
  previewPaused_ = false;
  previewClock_.restart();
  Q_EMIT previewPauseChanged(false);
  if (previewTimer_)
    previewTimer_->start();
  updatePreviewChrome();
}

void Browser::onPreviewTick() {
  if (!previewActive_)
    return;
  const qint64 totalMs =
      previewElapsedMs_ +
      (previewPaused_ ? 0 : previewClock_.elapsed());
  if (previewDurationSec_ > 0.0 && previewProgress_) {
    const int pct = static_cast<int>(qBound(
        0.0, totalMs / (previewDurationSec_ * 1000.0) * 100.0, 100.0));
    previewProgress_->setValue(pct);
    if (pct >= 100)
      stopPreview(true);
  }
}

void Browser::updatePreviewChrome() {
  if (!previewBtn_)
    return;
  if (previewActive_ && !previewPaused_) {
    // playing → the button pauses
    previewBtn_->setIcon(icons::make(icons::IconId::Stop, theme::kText, 16));
    previewBtn_->setToolTip(QStringLiteral("Pause preview"));
  } else {
    previewBtn_->setIcon(icons::make(icons::IconId::Preview, theme::kText, 16));
    previewBtn_->setToolTip(previewPaused_ ? QStringLiteral("Resume preview")
                                           : QStringLiteral("Play preview"));
  }
}

void Browser::buildPreview(const QString& path) {
  QFileInfo fi(path);
  if (!fi.exists() || !fi.isReadable()) {
    previewTitle_->setText(fi.fileName() + " (unreadable)");
    previewMeta_->setText("The preview could not be decoded.");
    previewDurationSec_ = 0.0;
    return;
  }
  misuc::audio::AudioBuffer audio;
  std::string err;
  if (!misuc::audio::readAudioFile(path.toStdString(), audio, err)) {
    previewTitle_->setText(fi.fileName() + " (decode failed)");
    previewMeta_->setText(QString::fromStdString(err));
    previewDurationSec_ = 0.0;
    return;
  }
  previewTitle_->setText(fi.fileName());
  QString fmt = QFileInfo(path).suffix().toUpper();
  if (fmt.isEmpty())
    fmt = QStringLiteral("UNKNOWN");
  QString meta = QString("%1 · %2 ch · %3 Hz · %4 s · %5 KB")
      .arg(fmt)
      .arg(audio.channels)
      .arg(qint64(audio.sampleRate))
      .arg(audio.durationSeconds(), 0, 'f', 2)
      .arg(qMax(1, int(fi.size() / 1024)));
  previewMeta_->setText(meta);
  previewDurationSec_ = audio.durationSeconds();
}

void Browser::selectScanFolder() {
  const QString dir = QFileDialog::getExistingDirectory(
      this, QStringLiteral("Scan Sample Pack Folder"),
      QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
  if (dir.isEmpty())
    return;
  indexPackFolder(dir);
}

void Browser::rebuildPackNodes() {
  if (!packsParent_)
    return;
  while (packsParent_->childCount() > 0)
    delete packsParent_->takeChild(0);
  for (const QString& dir : packDirs()) {
    const QString name = QDir(dir).dirName();
    auto* node = new QTreeWidgetItem(packsParent_, {name});
    const bool isKit = name.contains(QStringLiteral("kit"), Qt::CaseInsensitive);
    node->setIcon(0, icons::make(isKit ? icons::IconId::Instruments
                                       : icons::IconId::Samples,
                                 theme::kTextDim, 16));
    node->setData(0, Qt::UserRole, dir);
    node->setToolTip(0, dir);
  }
}

namespace {
QTreeWidgetItem* addSoundSubtree(QTreeWidgetItem* parent, const QString& dir) {
  auto* node = new QTreeWidgetItem(parent, {QDir(dir).dirName()});
  node->setIcon(0, icons::make(icons::IconId::Samples, theme::kTextDim, 16));
  node->setData(0, Qt::UserRole, dir);
  node->setToolTip(0, dir);
  const QDir d(dir);
  for (const QString& sub : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                        QDir::Name)) {
    addSoundSubtree(node, d.absoluteFilePath(sub));
  }
  return node;
}

// Counts the folder nodes nested under a Sounds-tree branch (used by the QA
// harness to verify deep folder structures are mirrored).
int countDescendantFolders(const QTreeWidgetItem* node) {
  int n = 0;
  for (int i = 0; i < node->childCount(); ++i) {
    ++n;
    n += countDescendantFolders(node->child(i));
  }
  return n;
}
} // namespace

void Browser::rebuildSoundNodes() {
  if (!soundsParent_)
    return;
  while (soundsParent_->childCount() > 0)
    delete soundsParent_->takeChild(0);
  ensureSoundsLibrary();
  QStringList roots = soundsLibraryRoots();
  QStringList seen;
  for (const QString& r : roots)
    if (!seen.contains(r))
      seen << r;
  for (const QString& r : seen) {
    const QString app =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    bool isAppLib = !app.isEmpty() &&
                    QDir(r) == QDir(app + QStringLiteral("/Sounds"));
    // The app-backed library nests its real folder structure directly under
    // the Sounds node (e.g. Sounds → Instruments → ...). Home folders appear
    // under their own name so custom packs stay reachable.
    const QString name = isAppLib ? QStringLiteral("Library")
                                  : QDir(r).dirName();
    auto* node = new QTreeWidgetItem(soundsParent_, {name});
    node->setIcon(0, icons::make(icons::IconId::Samples, theme::kTextDim, 16));
    node->setData(0, Qt::UserRole, r);
    node->setToolTip(0, r);
    if (isAppLib) {
      // The Library node directly mirrors the seeded folder structure; the
      // first level is NOT a fake category but the actual filesystem layout.
      const QDir lib(r);
      for (const QString& sub : lib.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                              QDir::Name)) {
        addSoundSubtree(node, lib.absoluteFilePath(sub));
      }
      // If only a top-level Library root is shown (no subfolders), selectable
      // files still list under it.
    }
  }
  // A flat folder layout that has files directly in a root still needs a node
  // pointing at that root (dedupe against the Library node above).
  for (const QString& r : seen) {
    const QString rootName = QDir(r).dirName();
    bool dup = false;
    for (int i = 0; i < soundsParent_->childCount(); ++i)
      if (soundsParent_->child(i)->text(0) == rootName ||
          (soundsParent_->child(i)->text(0) == QStringLiteral("Library") &&
           QDir(soundsParent_->child(i)->data(0, Qt::UserRole).toString()) ==
               QDir(r))) {
        dup = true;
        break;
      }
    if (dup)
      continue;
    auto* node = addSoundSubtree(soundsParent_, r);
    if (soundsParent_->childCount() > 0 &&
        soundsParent_->child(soundsParent_->childCount() - 1) == node)
      node->setText(0, rootName);
  }
}

void Browser::ensureSoundsLibrary() {
  const QString app =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (app.isEmpty())
    return;
  const QDir appSounds(app + QStringLiteral("/Sounds"));
  if (!appSounds.exists() && !QDir().mkpath(appSounds.absolutePath()))
    return;
  // Seed the guaranteed library with the real audio already provided to the
  // project (never synthesized): the demo pad lands in an Instruments folder
  // so the default UX (Sounds → Instruments → demo_pad.wav) works out of the
  // box. Files already present are left untouched.
  const QDir instruments(appSounds.absoluteFilePath(QStringLiteral("Instruments")));
  if (!instruments.exists() &&
      !QDir().mkpath(instruments.absolutePath()))
    return;
  {
    const QStringList existing =
        instruments.entryList(QDir::Files, QDir::Name);
    for (const QString& e : existing)
      if (audioExtensions().contains(QFileInfo(e).suffix().toLower()))
        return; // already seeded
  }
  const QStringList seeds = {
      QDir::home().filePath(
          QStringLiteral(".local/share/misuc/misuc/MisucDemo/demo_pad.wav")),
  };
  for (const QString& s : seeds) {
    if (!QFileInfo::exists(s))
      continue;
    if (QFile::copy(s, instruments.absoluteFilePath(QFileInfo(s).fileName())))
      break;
  }
}

void Browser::refreshSoundsLibrary() {
  ensureSoundsLibrary();
  // Preserve the currently selected sound scope across the node rebuild so the
  // refresh button keeps showing the same folder after rescanning it.
  QString keepPath;
  QTreeWidgetItem* cur = cats_ ? cats_->currentItem() : nullptr;
  if (cur) {
    const QString d = cur->data(0, Qt::UserRole).toString();
    if (QFileInfo(d).isDir())
      keepPath = QDir(d).absolutePath();
  }
  rebuildSoundNodes();
  if (keepPath.isEmpty()) {
    if (soundsParent_)
      cats_->setCurrentItem(soundsParent_);
  } else {
    QTreeWidgetItem* match = nullptr;
    for (int i = 0; i < soundsParent_->childCount() && !match; ++i) {
      QTreeWidgetItem* c = soundsParent_->child(i);
      const QString dd = c->data(0, Qt::UserRole).toString();
      if (QDir(dd).absolutePath() == keepPath) {
        match = c;
        break;
      }
      for (int j = 0; j < c->childCount() && !match; ++j) {
        QTreeWidgetItem* g = c->child(j);
        if (QDir(g->data(0, Qt::UserRole).toString()).absolutePath() ==
            keepPath)
          match = g;
      }
    }
    cats_->setCurrentItem(match ? match : soundsParent_);
  }
  onCategoryActivated();
}

void Browser::enqueueWaveform(const QString& path) {
  if (path.isEmpty() || waveformCache_.contains(path) ||
      waveformPending_.contains(path) || !QFileInfo(path).exists())
    return;
  if (waveformPending_.size() >= kWaveformPendingCap)
    return;
  waveformPending_.insert(path);
  waveformPool_->start(new WaveformTask(path, 96, this));
}

void Browser::onWaveformReady(const QString& path, const QVector<float>& bins) {
  waveformCache_.insert(path, bins);
  waveformPending_.remove(path);
  if (!files_)
    return;
  // Repaint only the shape column of the rows referencing this file.
  for (int i = 0; i < files_->topLevelItemCount(); ++i) {
    QTreeWidgetItem* it = files_->topLevelItem(i);
    if (!it)
      continue;
    if (it->data(0, Qt::UserRole).toString() == path ||
        it->data(2, Qt::UserRole).toString() == path) {
      const QModelIndex idx = files_->model()->index(i, 2);
      if (idx.isValid())
        files_->model()->dataChanged(idx, idx, {Qt::DecorationRole});
    }
  }
}

// --- persisted browser state -------------------------------------------------

QStringList Browser::packDirs() const {
  QSettings s;
  s.beginGroup(QStringLiteral("browser"));
  const QStringList dirs = s.value(QStringLiteral("packs")).toStringList();
  QStringList existing;
  for (const QString& d : dirs)
    if (QDir(d).exists() && !existing.contains(d))
      existing << QDir(d).absolutePath();
  return existing;
}

void Browser::savePackDirs(const QStringList& dirs) {
  QSettings s;
  s.beginGroup(QStringLiteral("browser"));
  s.setValue(QStringLiteral("packs"), dirs);
}

QStringList Browser::favourites() const {
  QSettings s;
  s.beginGroup(QStringLiteral("browser"));
  const QStringList favs = s.value(QStringLiteral("favourites")).toStringList();
  QStringList existing;
  for (const QString& f : favs)
    if (QFileInfo(f).isFile() && !existing.contains(f))
      existing << f;
  return existing;
}

bool Browser::isFavourite(const QString& path) const {
  return favourites().contains(path);
}

void Browser::setFavourite(const QString& path, bool on) {
  QStringList favs = favourites();
  const int i = favs.indexOf(path);
  if (on && i < 0) {
    favs << path;
  } else if (!on && i >= 0) {
    favs.removeAt(i);
  } else {
    return;
  }
  QSettings s;
  s.beginGroup(QStringLiteral("browser"));
  s.setValue(QStringLiteral("favourites"), favs);
}

QStringList Browser::recent() const {
  QSettings s;
  s.beginGroup(QStringLiteral("browser"));
  const QStringList rec = s.value(QStringLiteral("recent")).toStringList();
  QStringList existing;
  for (const QString& f : rec)
    if (QFileInfo(f).isFile() && !existing.contains(f))
      existing << f;
  return existing;
}

void Browser::saveRecent(const QStringList& recent) {
  QSettings s;
  s.beginGroup(QStringLiteral("browser"));
  s.setValue(QStringLiteral("recent"), recent);
}

void Browser::touchRecent(const QString& path) {
  QStringList rec = recent();
  rec.removeAll(path);
  rec.prepend(path);
  while (rec.size() > 12)
    rec.removeLast();
  saveRecent(rec);
  if (cats_->currentItem() && cats_->currentItem()->text(0) == "Recent")
    onCategoryActivated();
}

void Browser::toggleFavourite() {
  const QString p = currentFilePath();
  if (p.isEmpty())
    return;
  const bool now = !isFavourite(p);
  setFavourite(p, now);
  refreshFavIcon();
  // In the Favourites list an un-favourite removes the row immediately.
  if (!now && cats_->currentItem() &&
      cats_->currentItem()->text(0) == "Favourites")
    onCategoryActivated();
}

void Browser::refreshFavIcon() {
  if (!favBtn_)
    return;
  const QString p = currentFilePath();
  const bool on = !p.isEmpty() && isFavourite(p);
  favBtn_->setIcon(icons::make(icons::IconId::Favorites,
                               on ? theme::kAccent : theme::kTextDim, 16));
}

void Browser::setSessionSources(const QStringList& sourceFiles) {
  sessionSources_ = sourceFiles;
  if (cats_->currentItem() && cats_->currentItem()->text(0) == "Current Project")
    onCategoryActivated();
}

void Browser::refresh() {
  refreshSoundsLibrary();
}

int Browser::soundsFolderCount() const {
  return soundsParent_ ? soundsParent_->childCount() : 0;
}

int Browser::soundNodeTopCount() const {
  int n = 0;
  if (!soundsParent_)
    return 0;
  for (int i = 0; i < soundsParent_->childCount(); ++i)
    n += countDescendantFolders(soundsParent_->child(i));
  return n;
}

QStringList Browser::currentFilePaths() const {
  QStringList out;
  for (int i = 0; i < files_->topLevelItemCount(); ++i)
    out << files_->topLevelItem(i)->data(0, Qt::UserRole).toString();
  return out;
}

} // namespace misuc::ui