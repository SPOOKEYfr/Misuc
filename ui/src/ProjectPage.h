#pragma once

#include <QWidget>

#include <misuc/engine/project/Session.h>

class QLabel;
class QLineEdit;

namespace misuc::ui {

class HintManager;

// Project page: edit the session fields (name, artist, tempo). The Session is
// owned by the main window so the File menu and the page share one document;
// every committed edit emits edited() for the main window to push onto the
// undo stack and mark the document dirty.
class ProjectPage : public QWidget {
  Q_OBJECT
public:
  explicit ProjectPage(misuc::project::Session& session, QWidget* parent = nullptr);

  // Pull current session values into the editors (called after open/undo/redo).
  void refresh();

  // Registers hints for the project name/artist/tempo editors.
  void attachHints(HintManager* hints);

 signals:
  void edited();

 private:
  void commit();

  misuc::project::Session* session_ = nullptr;
  QLineEdit* nameEdit_ = nullptr;
  QLineEdit* artistEdit_ = nullptr;
  QLineEdit* tempoEdit_ = nullptr;
  HintManager* hints_ = nullptr;
};

} // namespace misuc::ui