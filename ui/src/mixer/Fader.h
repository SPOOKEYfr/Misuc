#pragma once

#include <QWidget>

class QMouseEvent;
class QWheelEvent;

namespace misuc::ui {

// Vertical channel fader: linear travel over [-60, +12] dB, mouse-wheel +-1 dB,
// double-click returns to 0 dB. Painted from the theme tokens; emits the dB
// value continuously so the strip and the engine stay in sync.
class Fader : public QWidget {
  Q_OBJECT
public:
  explicit Fader(QWidget* parent = nullptr);

  void setValueDb(float db);
  float valueDb() const { return valueDb_; }

  QSize sizeHint() const override { return QSize(44, 190); }
  QSize minimumSizeHint() const override { return QSize(36, 90); }

signals:
  void valueChanged(float db);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

private:
  float fromPos(int y) const;

  static constexpr float kMinDb = -60.f;
  static constexpr float kMaxDb = 12.f;

  float valueDb_ = 0.f;
  bool dragging_ = false;
};

} // namespace misuc::ui