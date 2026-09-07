#pragma once

#include <QWidget>

#include <misuc/engine/project/Session.h>

class QLabel;
class QLineEdit;

namespace misuc::ui {

// Project page: edit the session fields (name, tempo). The Session is owned by
// the main window so the File menu and the page share one document; every
// committed edit emits edited() for the main window to push onto the undo
// stack and mark the document dirty.
class ProjectPage : public QWidget {
  Q_OBJECT
public:
  explicit ProjectPage(misuc::project::Session& session, QWidget* parent = nullptr);

  // Pull current session values into the editors (called after open/undo/redo).
  void refresh();

signals:
  void edited();

private:
  void commit();

  misuc::project::Session* session_ = nullptr;
  QLineEdit* nameEdit_ = nullptr;
  QLineEdit* tempoEdit_ = nullptr;
};

} // namespace misuc::ui