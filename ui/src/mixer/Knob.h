#pragma once

#include <QWidget>

class QMouseEvent;
class QWheelEvent;

namespace misuc::ui {

// Compact rotary control for pan. Drag vertically (or wheel) to change the
// value; double-click snaps to the centre. Painted from the theme tokens.
class Knob : public QWidget {
  Q_OBJECT
public:
  explicit Knob(QWidget* parent = nullptr);

  void setRange(int min, int max);
  void setCenter(int center) {
    center_ = center;
    update();
  }
  void setValue(int v);
  int value() const { return value_; }
  QString title() const { return title_; }
  void setTitle(const QString& title);

  QSize sizeHint() const override { return QSize(52, 52); }

signals:
  void valueChanged(int v);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

private:
  int fromPos(int y) const;

  int min_ = -100, max_ = 100, center_ = 0, value_ = 0;
  QString title_;
  int lastY_ = 0;
  bool dragging_ = false;
};

} // namespace misuc::ui