#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>

class QApplication;

namespace misuc::ui {

// Central theme engine. Owns the semantic token set (as mutable `theme::k*`
// globals), provides the eight factory presets, applies a custom accent by
// recomputing the accent-derived tokens, persists the choice across runs,
// loads/saves `.misuctheme` files, and regenerates the app-wide palette and
// stylesheet. Emits `themeChanged()` after every effective change.
class ThemeEngine : public QObject {
  Q_OBJECT

public:
  static ThemeEngine& instance();

  struct Palette {
    QString name; // preset name, or "Custom" once edited
    QColor background;
    QColor panel;
    QColor panelRaised;
    QColor panelSunken;
    QColor border;
    QColor borderBright;
    QColor text;
    QColor textDim;
    QColor textFaint;
    QColor accent;
    QColor accentSoft; // accent blended into the panel (subtle tint fills)
    QColor ok;
    QColor danger;
    QColor meterDim;
    QColor meterTrack;
    QColor glass;
    QColor glassBorder;
    QColor glassHighlight;
  };

  const Palette& pal() const noexcept { return pal_; }
  QString presetName() const { return pal_.name; }

  QStringList presetNames() const;
  bool setPreset(const QString& name);           // false = unknown preset
  void setAccent(const QColor& accent);           // -> "Custom"
  QColor accent() const noexcept { return pal_.accent; }

  bool glassEnabled() const noexcept { return glass_; }
  void setGlassEnabled(bool on);

  // Full application stylesheet generated from the current palette.
  QString buildStyleSheet() const;
  // QSS for a glass / solid raised panel (used by mixer strips).
  QString glassPanelStyle(bool glass) const;

  // Apply palette + stylesheet to the running app and notify widgets.
  void apply(QApplication& app);

  // .misuctheme round-trip (JSON).
  bool loadFromFile(const QString& path, QString& err);
  bool saveToFile(const QString& path, QString& err) const;
  QByteArray toJson() const;
  bool fromJson(const QByteArray& json, QString& err);

  // Persist the current choice (preset or custom accent) via QSettings.
  void loadPersisted();
  void savePersisted() const;

signals:
  void themeChanged();

private:
  explicit ThemeEngine(QObject* parent = nullptr);
  static Palette presetPalette(const QString& name);
  void adopt(const Palette& p);
  void pushToTokens() const; // write pal_ into the theme::k* globals

  Palette pal_;
  // Last real preset base (never "Custom") a custom accent was layered on.
  // Persists alongside a custom accent so restart can reconstruct the exact
  // palette; kept purely in-memory, reset by every adopt() of a real preset.
  QString basePreset_;
  bool glass_ = true;
  bool firstApply_ = true;
  bool applying_ = false; // re-entrancy guard: apply() must not nest
};

} // namespace misuc::ui