#pragma once

#include <QWidget>

#include <misuc/engine/project/Session.h>

namespace misuc::engine {
class AudioEngine;
}

class QLabel;
class QLineEdit;

namespace misuc::ui {

// Project page: edit the .misuc session fields and persist to disk. Real
// JSON serialization through the engine Session model.
class ProjectPage : public QWidget {
  Q_OBJECT
public:
  explicit ProjectPage(misuc::engine::AudioEngine& engine, QWidget* parent = nullptr);

private Q_SLOTS:
  void save();
  void loadPicked(const QString& path);

private:
  misuc::engine::AudioEngine& engine_;
  misuc::project::Session session_;
  QLineEdit* nameEdit_ = nullptr;
  QLineEdit* tempoEdit_ = nullptr;
  QLabel* status_ = nullptr;
};

} // namespace misuc::ui