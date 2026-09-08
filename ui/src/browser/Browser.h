#pragma once

#include <QLabel>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QVBoxLayout;
class QToolButton;

namespace misuc::ui {

class HintManager;

// Browser workspace (master spec §26-27): category tree on the left, real file
// listing on the right, a preview strip at the bottom. Audio files are scanned
// from the platform music/homedir locations ("Samples/Audio"), project-audio
// comes from the live Session's clip sources ("Current Project"), and the
// Instruments/Effects/MIDI categories list the built-in studio content.
// Selecting an audio file loads a real preview (waveform + metadata via the
// engine's decoder); Preview/Insert act on it and drag-to-Playlist is enabled.
class Browser : public QWidget {
  Q_OBJECT
public:
  explicit Browser(QWidget* parent = nullptr);

  void setSessionSources(const QStringList& sourceFiles);
  void setInsertTarget(bool canInsertStyle);
  void refresh();

  // Registers the icon-only chrome with the global hint manager.
  void attachHints(HintManager* hints);

  // Preview strip control access for the hint bar's live values.
  QString currentPreviewName() const { return previewTitle_->text(); }
  bool hasPreviewSelection() const {
    return previewTitle_ && !previewTitle_->text().startsWith(
                                QStringLiteral("Select an audio"));
  }

signals:
  void insertSample(const QString& path);
  void hint(const QString& text);
  void previewRequested();

protected:
  void populateSamples();

private:
  void onCategoryActivated();
  void onFileActivated();
  void selectFileItem(QTreeWidgetItem*);
  void buildPreview(const QString& path);

  QTreeWidget* cats_ = nullptr;
  QTreeWidget* files_ = nullptr;
  QVBoxLayout* previewHost_ = nullptr;
  QLabel* previewTitle_ = nullptr;
  QLabel* previewMeta_ = nullptr;
  QToolButton* previewBtn_ = nullptr;
  QToolButton* insertBtn_ = nullptr;
  QLineEdit* searchBox_ = nullptr;
  QStringList sessionSources_;
};

} // namespace misuc::ui