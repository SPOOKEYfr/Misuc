#pragma once

#include <QDialog>
#include <QString>

#include <string>

class QVBoxLayout;
class QWidget;

namespace misuc::project {
class Session;
} // namespace misuc::project

namespace misuc::ui {

// Modal editor for a source's parallel sends (Comp19 / master spec §21): one
// row per target bus (exluding the source itself when it is a bus) with an
// enable checkbox and a level slider. Operates directly on the Session (sends
// are add/remove-per-source), so ids stay authoritative. Callers re-sync the
// engine afterwards and treat an accept as a project edit.
class SendDialog : public QDialog {
  Q_OBJECT
public:
  SendDialog(misuc::project::Session* session, const QString& sourceTrackId,
             const QString& sourceBusId, QWidget* parent = nullptr);
};

// Modal editor for a bus's post-fader insert chain (Comp19): add/remove the
// built-in SC compressor and pick its sidechain key source bus. Also edits the
// session directly so the sidechainSource round-trips with the document. The
// row rebuild is a member (not a ctor local std::function) so the button
// handlers that re-invoke it outlive the constructor.
class BusFxDialog : public QDialog {
  Q_OBJECT
public:
  BusFxDialog(misuc::project::Session* session, const QString& busId,
              QWidget* parent = nullptr);

private:
  void rebuildRows();

  misuc::project::Session* session_;
  std::string busId_;
  QVBoxLayout* rows_ = nullptr;
};

} // namespace misuc::ui