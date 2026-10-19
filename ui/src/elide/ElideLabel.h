#pragma once

#include <QLabel>
#include <QResizeEvent>
#include <QSize>

namespace misuc::ui {

// QLabel that always keeps its full content reachable while never painting
// into its neighbours (Comp07: dock/tab panels must stay correct at narrow
// widths). The visible text elides with an ellipsis whenever the widget is
// narrower than the content, and the full text is exposed through a tooltip
// precisely when something has been elided. Because the minimum width is a
// small fixed value (not the font-metrics width), this label can shrink and
// its surrounding layout can compress instead of forcing a too-wide dock.
class ElideLabel : public QLabel {
  // Plain QObject subclass (no signals/slots): header-only, no moc required.
public:
  explicit ElideLabel(QWidget* parent = nullptr) : QLabel(parent) {
    setTextInteractionFlags(Qt::TextSelectableByMouse);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setWordWrap(false);
  }

  // QLabel::setText/text are not virtual; callers must hold an ElideLabel* so
  // the elided view rebuilds from the full text.
  void setText(const QString& text) { reElideFrom(text); }
  QString text() const { return fullText_; }
  QString fullText() const { return fullText_; }
  bool isElided() const {
    return !fullText_.isEmpty() &&
           QLabel::text() != fullText_;
  }

  QSize sizeHint() const override {
    // Horizontal control belongs to the surrounding layout, not the text.
    return QSize(40, QLabel::sizeHint().height());
  }
  QSize minimumSizeHint() const override {
    return QSize(0, QLabel::sizeHint().height());
  }

protected:
  void resizeEvent(QResizeEvent*) override { reElide(); }

private:
  void reElideFrom(const QString& full) {
    fullText_ = full;
    reElide();
  }
  void reElide() {
    if (fullText_.isEmpty()) {
      QLabel::setText(QString());
      setToolTip(QString());
      return;
    }
    const int w = width();
    const QString shown =
        w <= 0 ? fullText_
               : fontMetrics().elidedText(fullText_, Qt::ElideRight, w);
    QLabel::setText(shown);
    setToolTip(shown == fullText_ ? QString() : fullText_);
  }

  QString fullText_;
};

} // namespace misuc::ui