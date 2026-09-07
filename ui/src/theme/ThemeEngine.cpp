#include "ThemeEngine.h"

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QSettings>

#include "../Theme.h"

namespace misuc::ui {

ThemeEngine& ThemeEngine::instance() {
  static ThemeEngine engine; // thread-safe C++11 magic static
  return engine;
}

namespace {

struct PresetRecord {
  const char* name;
  const char* background;
  const char* panel;
  const char* panelRaised;
  const char* panelSunken;
  const char* border;
  const char* borderBright;
  const char* text;
  const char* textDim;
  const char* textFaint;
  const char* accent;
  const char* ok;
  const char* danger;
};

const PresetRecord kPresets[] = {
    // Dark, warm studio baseline. This is the long-standing Misuc look.
    {"Misuc Graphite", "#0d121b", "#151c27", "#1c2531", "#0a0e15",
     "#28303e", "#354054", "#e8edf4", "#8a94a6", "#5b6472",
     "#ff6b4a", "#4ade80", "#ff5c5c"},
    // Cool midnight blue, blue accent.
    {"Midnight Blue", "#081120", "#101d31", "#162642", "#050b14",
     "#24344f", "#314566", "#eaf1fb", "#8fa4c3", "#5c6d8a",
     "#4f8bff", "#4ade80", "#ff5c5c"},
    // Deep violet creative studio.
    {"Violet Studio", "#120d1e", "#1c1530", "#241b3d", "#0a0713",
     "#312a4d", "#3f3560", "#f0ebfa", "#a294c4", "#6b5f8c",
     "#a970ff", "#4ade80", "#ff5c5c"},
    // Electric cyan on near-black teal.
    {"Electric Cyan", "#07181c", "#0e242b", "#14303a", "#040f12",
     "#234149", "#2f5660", "#eafcff", "#87b6c4", "#55727d",
     "#22d3ee", "#4ade80", "#ff5c5c"},
    // Warm crimson studio.
    {"Crimson Studio", "#1a0e12", "#24141b", "#2f1a22", "#110709",
     "#40282f", "#52323b", "#faecf0", "#bf909d", "#8a5f6d",
     "#ff5470", "#4ade80", "#ff5c5c"},
    // Emerald studio green.
    {"Emerald Studio", "#081710", "#0e2317", "#142e1e", "#040e09",
     "#1f3d28", "#2a5235", "#eafaf0", "#8cc0a0", "#5b8373",
     "#34d399", "#4ade80", "#ff5c5c"},
    // Warm amber on near-black.
    {"Amber Studio", "#191106", "#241a0c", "#2f2312", "#0f0a03",
     "#40301a", "#524023", "#faf3e8", "#c4a981", "#8f7a56",
     "#ffb454", "#4ade80", "#ff5c5c"},
    // Deepest blacks for OLED panels; coral accent stays on-brand.
    {"OLED Black", "#000000", "#0a0a0b", "#121214", "#000000",
     "#1d1d20", "#2a2a2e", "#e9e9ec", "#7f7f88", "#52525a",
     "#ff6b4a", "#4ade80", "#ff5c5c"},
};

constexpr size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

// Blend `over` on top of `under` with the given coverage.
QColor blend(const QColor& under, const QColor& over, int alpha255) {
  const int a = over.alpha() * alpha255 / 255;
  const int ia = 255 - a;
  return QColor((over.red() * a + under.red() * ia) / 255,
                (over.green() * a + under.green() * ia) / 255,
                (over.blue() * a + under.blue() * ia) / 255);
}

} // namespace

ThemeEngine::ThemeEngine(QObject* parent) : QObject(parent) {
  adopt(presetPalette(QStringLiteral("Misuc Graphite")));
}

ThemeEngine::Palette ThemeEngine::presetPalette(const QString& name) {
  Palette p;
  p.name = name;
  for (const PresetRecord& r : kPresets) {
    if (name != QLatin1String(r.name))
      continue;
    p.background = QColor(r.background);
    p.panel = QColor(r.panel);
    p.panelRaised = QColor(r.panelRaised);
    p.panelSunken = QColor(r.panelSunken);
    p.border = QColor(r.border);
    p.borderBright = QColor(r.borderBright);
    p.text = QColor(r.text);
    p.textDim = QColor(r.textDim);
    p.textFaint = QColor(r.textFaint);
    p.accent = QColor(r.accent);
    p.ok = QColor(r.ok);
    p.danger = QColor(r.danger);
    p.meterDim = blend(p.background, QColor(r.panel), 200);
    p.meterTrack = blend(p.background, QColor(r.border), 255);
    p.glass = QColor(r.panelRaised);
    p.glass.setAlpha(200);
    p.glassBorder = QColor(r.borderBright);
    p.glassBorder.setAlpha(70);
    p.glassHighlight = QColor(255, 255, 255, 26);
    return p;
  }
  return presetPalette(QStringLiteral("Misuc Graphite")); // fallback
}

QStringList ThemeEngine::presetNames() const {
  QStringList names;
  for (const PresetRecord& r : kPresets)
    names << QLatin1String(r.name);
  return names;
}

bool ThemeEngine::setPreset(const QString& name) {
  for (const PresetRecord& r : kPresets) {
    if (name == QLatin1String(r.name)) {
      adopt(presetPalette(name));
      return true;
    }
  }
  return false;
}

void ThemeEngine::setAccent(const QColor& accent) {
  Palette p = pal_;
  p.accent = accent;
  p.name = QStringLiteral("Custom");
  adopt(p);
  savePersisted();
}

void ThemeEngine::setGlassEnabled(bool on) {
  if (glass_ == on)
    return;
  glass_ = on;
  emit themeChanged();
}

void ThemeEngine::adopt(const Palette& p) {
  pal_ = p;
  pal_.accentSoft = blend(pal_.panel, pal_.accent, 48);
  pushToTokens();
}

void ThemeEngine::pushToTokens() const {
  theme::kBackground = pal_.background;
  theme::kPanel = pal_.panel;
  theme::kPanelRaised = pal_.panelRaised;
  theme::kPanelSunken = pal_.panelSunken;
  theme::kBorder = pal_.border;
  theme::kBorderBright = pal_.borderBright;
  theme::kText = pal_.text;
  theme::kTextDim = pal_.textDim;
  theme::kTextFaint = pal_.textFaint;
  theme::kAccent = pal_.accent;
  theme::kAccentPressed = pal_.accent.darker(120);
  theme::kAccentSoft = pal_.accentSoft;
  theme::kOk = pal_.ok;
  theme::kDanger = pal_.danger;
  theme::kMeterDim = pal_.meterDim;
  theme::kMeterTrack = pal_.meterTrack;
  theme::kGlass = pal_.glass;
  theme::kGlassBorder = pal_.glassBorder;
  theme::kGlassHighlight = pal_.glassHighlight;
}

QString ThemeEngine::buildStyleSheet() const {
  const QString t = theme::kText.name();
  const QString bg = theme::kBackground.name();
  const QString dim = theme::kTextDim.name();
  const QString raised = theme::kPanelRaised.name();
  const QString border = theme::kBorder.name();
  const QString panel = theme::kPanel.name();
  const QString accent = theme::kAccent.name();
  const QString sunken = theme::kPanelSunken.name();

  return QStringLiteral(R"(
    QWidget { color: %1; }
    QMainWindow, QWidget#central { background-color: %2; }
    QPushButton {
      background-color: %4; color: %1; border: 1px solid %5;
      border-radius: 6px; padding: 8px 16px; font-weight: 500;
    }
    QPushButton:hover { background-color: #243041; }
    QPushButton:pressed { background-color: #10151e; }
    QPushButton:disabled { color: %3; border-color: %5; }
    QPushButton#navButton {
      text-align: left; padding: 10px 12px; color: %3;
      background: transparent; border: none; border-radius: 6px;
    }
    QPushButton#navButton:checked {
      background-color: %6; color: %1; border: none;
    }
    QLineEdit {
      background-color: %8; color: %1; border: 1px solid %5;
      border-radius: 6px; padding: 6px 10px; selection-background-color: %7;
    }
    QComboBox {
      background-color: %8; color: %1; border: 1px solid %5;
      border-radius: 6px; padding: 6px 10px;
    }
    QComboBox QAbstractItemView {
      background-color: %4; color: %1; border: 1px solid %5;
      selection-background-color: %7; selection-color: black;
    }
    QGroupBox {
      border: 1px solid %5; border-radius: 8px; margin-top: 12px;
      padding-top: 6px; font-weight: 600; color: %3;
    }
    QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }
    QSlider::groove:horizontal {
      height: 4px; background: %5; border-radius: 2px;
    }
    QSlider::handle:horizontal {
      width: 14px; margin: -5px 0; border-radius: 7px;
      background: %7; border: none;
    }
    QToolTip {
      background-color: %4; color: %1; border: 1px solid %5;
      padding: 4px 8px;
    }
    QMenuBar { background: %2; color: %1; padding: 2px 6px; }
    QMenuBar::item { background: transparent; padding: 6px 12px; border-radius: 6px; }
    QMenuBar::item:selected { background: %6; }
    QMenu {
      background-color: %4; color: %1; border: 1px solid %5;
      border-radius: 8px; padding: 6px;
    }
    QMenu::item { padding: 6px 22px; border-radius: 4px; }
    QMenu::item:selected { background-color: %7; color: black; }
    QMenu::item:disabled { color: %3; }
    QMenu::separator { height: 1px; background: %5; margin: 6px 8px; }
    QPushButton#smallButton { padding: 5px 12px; font-weight: 500; }
    QScrollArea { background: %8; border: none; }
    QScrollBar:vertical { background: %2; width: 10px; margin: 0; }
    QScrollBar::handle:vertical {
      background: %6; min-height: 24px; border-radius: 5px;
    }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    QScrollBar:horizontal { background: %2; height: 10px; margin: 0; }
    QScrollBar::handle:horizontal {
      background: %6; min-width: 24px; border-radius: 5px;
    }
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
    QCheckBox { color: %1; spacing: 8px; }
    QCheckBox::indicator {
      width: 16px; height: 16px; border: 1px solid %5;
      border-radius: 4px; background: %8;
    }
    QCheckBox::indicator:checked { background: %7; border-color: %7; }
    QListWidget::item { color: %1; border-radius: 6px; margin: 2px; }
    QListWidget::item:selected { background-color: %6; color: %1; }
    QListWidget::item:hover { background-color: %4; }
  )")
      .arg(t, bg, dim, raised, border, panel, accent, sunken);
}

QString ThemeEngine::glassPanelStyle(bool glassLine) const {
  const QColor surf = glass_ ? theme::kGlass : theme::kPanelRaised;
  const QColor edge = glass_ ? theme::kGlassBorder : theme::kBorder;
  const QColor top = glass_ ? theme::kGlassHighlight : theme::kBorderBright;
  QString style = QStringLiteral(
      "background-color: %1; border: 1px solid %2; border-radius: 10px;");
  style = style.arg(surf.name(QColor::HexArgb), edge.name(QColor::HexArgb));
  if (glassLine) {
    style += QStringLiteral(
        "border-top: 1px solid %1;");
    style = style.arg(top.name(QColor::HexArgb));
  }
  return style;
}

void ThemeEngine::apply(QApplication& app) {
  QPalette pal = app.palette();
  pal.setColor(QPalette::Window, theme::kBackground);
  pal.setColor(QPalette::WindowText, theme::kText);
  pal.setColor(QPalette::Base, theme::kPanelSunken);
  pal.setColor(QPalette::AlternateBase, theme::kPanel);
  pal.setColor(QPalette::Text, theme::kText);
  pal.setColor(QPalette::Button, theme::kPanelRaised);
  pal.setColor(QPalette::ButtonText, theme::kText);
  pal.setColor(QPalette::ToolTipBase, theme::kPanelRaised);
  pal.setColor(QPalette::ToolTipText, theme::kText);
  pal.setColor(QPalette::Highlight, theme::kAccent);
  pal.setColor(QPalette::HighlightedText, Qt::black);
  pal.setColor(QPalette::PlaceholderText, theme::kTextFaint);
  app.setPalette(pal);
  app.setStyleSheet(buildStyleSheet());
  if (firstApply_) {
    firstApply_ = false;
    return; // startup; widgets don't exist yet to notify
  }
  emit themeChanged();
}

QByteArray ThemeEngine::toJson() const {
  QJsonObject o;
  o["name"] = pal_.name;
  o["accent"] = pal_.accent.name();
  o["background"] = pal_.background.name();
  o["panel"] = pal_.panel.name();
  o["panelRaised"] = pal_.panelRaised.name();
  o["panelSunken"] = pal_.panelSunken.name();
  o["border"] = pal_.border.name();
  o["borderBright"] = pal_.borderBright.name();
  o["text"] = pal_.text.name();
  o["textDim"] = pal_.textDim.name();
  o["textFaint"] = pal_.textFaint.name();
  o["ok"] = pal_.ok.name();
  o["danger"] = pal_.danger.name();
  o["meterDim"] = pal_.meterDim.name();
  o["meterTrack"] = pal_.meterTrack.name();
  return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

namespace {
bool setIf(const QJsonObject& o, const char* key, QColor& out) {
  if (!o.contains(QLatin1String(key)))
    return false;
  QColor c(o.value(QLatin1String(key)).toString());
  if (!c.isValid())
    return false;
  out = c;
  return true;
}
} // namespace

bool ThemeEngine::fromJson(const QByteArray& json, QString& err) {
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
    err = QStringLiteral("not a valid theme JSON: %1").arg(pe.errorString());
    return false;
  }
  const QJsonObject o = doc.object();
  Palette p = pal_;
  p.name = o.value(QStringLiteral("name")).toString(p.name);
  if (o.contains(QStringLiteral("accent"))) {
    const QColor c(o.value(QStringLiteral("accent")).toString());
    if (c.isValid())
      p.accent = c;
  }
  setIf(o, "background", p.background);
  setIf(o, "panel", p.panel);
  setIf(o, "panelRaised", p.panelRaised);
  setIf(o, "panelSunken", p.panelSunken);
  setIf(o, "border", p.border);
  setIf(o, "borderBright", p.borderBright);
  setIf(o, "text", p.text);
  setIf(o, "textDim", p.textDim);
  setIf(o, "textFaint", p.textFaint);
  setIf(o, "ok", p.ok);
  setIf(o, "danger", p.danger);
  setIf(o, "meterDim", p.meterDim);
  setIf(o, "meterTrack", p.meterTrack);
  adopt(p);
  return true;
}

bool ThemeEngine::loadFromFile(const QString& path, QString& err) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    err = QStringLiteral("cannot open theme file: %1").arg(f.errorString());
    return false;
  }
  const bool ok = fromJson(f.readAll(), err);
  if (ok)
    savePersisted();
  return ok;
}

bool ThemeEngine::saveToFile(const QString& path, QString& err) const {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    err = QStringLiteral("cannot write theme file: %1").arg(f.errorString());
    return false;
  }
  f.write(toJson());
  f.close();
  return true;
}

void ThemeEngine::loadPersisted() {
  QSettings s;
  const QString preset = s.value(QStringLiteral("theme/preset")).toString();
  const QString accentHex = s.value(QStringLiteral("theme/accent")).toString();
  if (!preset.isEmpty() && setPreset(preset))
    return;
  // Only a stored accent survives across presets.
  if (setPreset(QStringLiteral("Misuc Graphite"))) {
    if (!accentHex.isEmpty()) {
      const QColor c(accentHex);
      if (c.isValid()) {
        pal_.accent = c;
        adopt(pal_);
      }
    }
  }
}

void ThemeEngine::savePersisted() const {
  QSettings s;
  s.setValue(QStringLiteral("theme/preset"), pal_.name);
  if (pal_.name == QStringLiteral("Custom"))
    s.setValue(QStringLiteral("theme/accent"), pal_.accent.name());
  else
    s.remove(QStringLiteral("theme/accent"));
}

} // namespace misuc::ui