// Regenerates the on-disk Misuc icon set (hicolor theme + bundled app icon)
// from the shared vector logo. Run from the repo root:
//   tools/rendericons
// Outputs:
//   assets/hicolor/<size>x<size>/apps/misuc.png            (desktop theme)
//   ui/resources/icons/misuc-256.png                      (bundled app icon)
//   ui/resources/icons/misuc-512.png                      (scalable-capable)
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPixmap>

#include "branding/Logo.h"

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);

  const QString root = QCoreApplication::applicationDirPath();
  const QDir cwd = QDir::current();
  const QString hicolor = cwd.filePath(QStringLiteral("assets/hicolor"));
  if (!QDir().mkpath(hicolor)) {
    qWarning("cannot create %s", qPrintable(hicolor));
    return 1;
  }

  const int sizes[] = {16, 22, 24, 32, 48, 64, 128, 256, 512};
  QColor accent = misuc::ui::logoAccent();
  QColor soft = misuc::ui::logoAccentSoft();
  for (int s : sizes) {
    const QPixmap pm =
        misuc::ui::logoPixmap(s, accent, soft);
    QString dir = cwd.filePath(QStringLiteral("assets/hicolor/%1x%1/apps")
                                   .arg(s));
    if (!QDir().mkpath(dir))
      return 1;
    const QString out = QDir(dir).filePath(QStringLiteral("misuc.png"));
    if (!pm.save(out, "PNG")) {
      qWarning("failed to write %s", qPrintable(out));
      return 1;
    }
    qInfo("wrote %s", qPrintable(out));
  }

  QDir().mkpath(cwd.filePath(QStringLiteral("ui/resources/icons")));
  const QString icon =
      cwd.filePath(QStringLiteral("ui/resources/icons/misuc-256.png"));
  const QPixmap pm = misuc::ui::logoPixmap(256, accent, soft);
  if (!pm.save(icon, "PNG"))
    return 1;
  qInfo("wrote %s", qPrintable(icon));
  const QString icon512 =
      cwd.filePath(QStringLiteral("ui/resources/icons/misuc-512.png"));
  const QPixmap pm512 = misuc::ui::logoPixmap(512, accent, soft);
  if (!pm512.save(icon512, "PNG"))
    return 1;
  qInfo("wrote %s", qPrintable(icon512));
  return 0;
}