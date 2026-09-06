#include "Theme.h"

namespace misuc::ui::theme {

QFont uiFont(int pointSize, QFont::Weight weight) {
  QFont f;
  f.setFamily(QStringLiteral("Inter"));
  f.setPointSize(pointSize);
  f.setWeight(weight);
  return f;
}

QFont monoFont(int pointSize) {
  QFont f;
  f.setFamily(QStringLiteral("JetBrains Mono"));
  f.setPointSize(pointSize);
  f.setWeight(QFont::Medium);
  return f;
}

} // namespace misuc::ui::theme