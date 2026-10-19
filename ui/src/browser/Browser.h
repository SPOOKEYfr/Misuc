#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QLabel>
#include "elide/ElideLabel.h"
#include <QSet>
#include <QSlider>
#include <QTreeWidget>
#include <QVector>
#include <QWidget>

class QProgressBar;
class QSlider;
class QThreadPool;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QVBoxLayout;
class QToolButton;

namespace misuc::ui {

class HintManager;

// Browser workspace (master spec §26-27 and §4): category tree on the left,
// real file listing on the right, a preview strip at the bottom. Audio files
// are scanned from a default "Sounds" library (a guaranteed, filesystem-backed
// sounds folder under the app data directory, seeded with the project's real
// audio assets, plus the user's home Sounds/Samples folders), project-audio
// comes from the live Session's clip sources ("Current Project"), and user
// sample packs are indexed under a persistent "Packs" section, with Favourites
// and Recent persisted alongside. Selecting an audio file decodes a real
// waveform thumbnail (asynchronously, cached, 96 bins per row) plus metadata,
// and the preview strip plays it through the engine's one-shot sample preview
// with play/pause/stop and volume. Audio files drag into the Channel Rack
// (new sampler channel) or the Playlist (real clip) via standard MIME at the
// drop position. The search field filters the whole index by filename, and
// refresh/rescan picks up newly added/removed sounds without a rebuild.
class Browser : public QWidget {
  Q_OBJECT
public:
  explicit Browser(QWidget* parent = nullptr);
  ~Browser() override;

  void setSessionSources(const QStringList& sourceFiles);
  void setInsertTarget(bool canInsertStyle);
  // Rescans the Sounds library roots and the current file listing (used by the
  // refresh button and after external changes).
  void refresh();

  // Registers the icon-only chrome with the global hint manager.
  void attachHints(HintManager* hints);

  // Indexes a sample-pack folder: adds it to the persisted pack list (if not
  // already present), reveals it under "Packs" and refreshes the current
  // listing. The Scan-Pack toolbar button runs this exact code path.
  bool indexPackFolder(const QString& dir);

  // Preview strip control access for the hint bar's live values.
  QString currentPreviewName() const { return previewTitle_->text(); }
  bool hasPreviewSelection() const {
    return previewTitle_ && !previewTitle_->text().startsWith(
                                QStringLiteral("Select an audio"));
  }
  // Current preview volume (0..2 linear; slider is 0..100 using 1.0 at 50).
  double previewVolume() const noexcept {
    return previewVolumeSlider_ ? previewVolumeSlider_->value() / 50.0 : 1.0;
  }
  // Test/QA helpers ------------------------------------------------------
  int soundsFolderCount() const;
  int fileCount() const { return files_->topLevelItemCount(); }
  QStringList currentFilePaths() const;
  // Cached waveform bins for a file (empty until the async pass finishes).
  QVector<float> waveformBins(const QString& path) const {
    return waveformCache_.value(path);
  }
  int soundNodeTopCount() const;

signals:
  void insertSample(const QString& path);
  void hint(const QString& text);
  // Real audio audition: the selected sample file should be played through the
  // engine's one-shot sample preview pipeline (not a synthesized beep).
  void previewFile(const QString& path);
  // Preview strip transport/volume → the engine's preview controls.
  void previewPauseChanged(bool paused);
  void previewStopRequested();
  void previewVolumeChanged(double volume);

public slots:
  // Async waveform completion (called by the worker task on the GUI thread).
  void onWaveformReady(const QString& path, const QVector<float>& bins);

protected:
  void populateSamples();

private:
  void onCategoryActivated();
  void onFileActivated();
  void selectFileItem(QTreeWidgetItem*);
  void buildPreview(const QString& path);
  void selectScanFolder();
  void onSearchTextChanged(const QString& text);
  void toggleFavourite();
  void rebuildPackNodes();
  void rebuildSoundNodes();
  QString currentFilePath() const;
  void refreshSoundsLibrary();
  void ensureSoundsLibrary();
  void enqueueWaveform(const QString& path);
  // Preview strip state machine (the engine owns the audio; the strip tracks
  // an honest UI-clock position for progress).
  void startPreview(const QString& path);
  void stopPreview(bool notifyEngine);
  void resumePreview();
  void onPreviewTick();
  void updatePreviewChrome();
  bool eventFilter(QObject* watched, QEvent* event) override;

  // Persisted browser state (QSettings "browser" group).
  QStringList packDirs() const;
  void savePackDirs(const QStringList& dirs);
  QStringList favourites() const;
  bool isFavourite(const QString& path) const;
  void setFavourite(const QString& path, bool on);
  QStringList recent() const;
  void saveRecent(const QStringList& recent);
  void touchRecent(const QString& path);
  void refreshFavIcon();

  QTreeWidget* cats_ = nullptr;
  QTreeWidget* files_ = nullptr;
  QVBoxLayout* previewHost_ = nullptr;
  ElideLabel* previewTitle_ = nullptr;
  ElideLabel* previewMeta_ = nullptr;
  QToolButton* previewBtn_ = nullptr;
  QToolButton* previewStopBtn_ = nullptr;
  QToolButton* favBtn_ = nullptr;
  QToolButton* insertBtn_ = nullptr;
  QToolButton* scanBtn_ = nullptr;
  QToolButton* refreshBtn_ = nullptr;
  QSlider* previewVolumeSlider_ = nullptr;
  QProgressBar* previewProgress_ = nullptr;
  QTimer* previewTimer_ = nullptr;
  QLineEdit* searchBox_ = nullptr;
  QTreeWidgetItem* packsParent_ = nullptr;
  QTreeWidgetItem* soundsParent_ = nullptr;
  QStringList sessionSources_;
  // Waveform thumbnails: async-computed, cached per path, never rebuilt while
  // the same file is visible. Work runs on a small dedicated pool so browsing a
  // huge sample library (e.g. packs on a large shared NTFS partition) can never
  // saturate every CPU core and starve the audio thread; the pending set is
  // capped to bound memory and enqueue volume.
  QHash<QString, QVector<float>> waveformCache_;
  QSet<QString> waveformPending_;
  QThreadPool* waveformPool_ = nullptr;
  // Preview strip playback state (UI-clock driven; the engine owns the audio).
  bool previewActive_ = false;
  bool previewPaused_ = false;
  qint64 previewElapsedMs_ = 0;
  qint64 previewPausedMs_ = 0;
  double previewDurationSec_ = 0.0;
  QElapsedTimer previewClock_;
};

} // namespace misuc::ui