#include <memory>

#include <QApplication>

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/PipeWireBackend.h>

#include "MainWindow.h"
#include "Theme.h"

using misuc::engine::AudioEngine;
using misuc::engine::PipeWireBackend;

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Misuc"));
  QApplication::setOrganizationName(QStringLiteral("Misuc"));

  app.setStyle(QStringLiteral("Fusion"));

  // Global palette + stylesheet drive the look; the pages add their own
  // details on top.
  QPalette pal = app.palette();
  pal.setColor(QPalette::Window, misuc::ui::theme::kBackground);
  pal.setColor(QPalette::WindowText, misuc::ui::theme::kText);
  pal.setColor(QPalette::Base, misuc::ui::theme::kPanelSunken);
  pal.setColor(QPalette::AlternateBase, misuc::ui::theme::kPanel);
  pal.setColor(QPalette::Text, misuc::ui::theme::kText);
  pal.setColor(QPalette::Button, misuc::ui::theme::kPanelRaised);
  pal.setColor(QPalette::ButtonText, misuc::ui::theme::kText);
  pal.setColor(QPalette::ToolTipBase, misuc::ui::theme::kPanelRaised);
  pal.setColor(QPalette::ToolTipText, misuc::ui::theme::kText);
  pal.setColor(QPalette::Highlight, misuc::ui::theme::kAccent);
  pal.setColor(QPalette::HighlightedText, Qt::black);
  pal.setColor(QPalette::PlaceholderText, misuc::ui::theme::kTextFaint);
  app.setPalette(pal);

  app.setStyleSheet(QStringLiteral(R"(
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
  )")
      .arg(misuc::ui::theme::kText.name())
      .arg(misuc::ui::theme::kBackground.name())
      .arg(misuc::ui::theme::kTextDim.name())
      .arg(misuc::ui::theme::kPanelRaised.name())
      .arg(misuc::ui::theme::kBorder.name())
      .arg(misuc::ui::theme::kPanel.name())
      .arg(misuc::ui::theme::kAccent.name())
      .arg(misuc::ui::theme::kPanelSunken.name()));

  auto engine = std::make_unique<AudioEngine>(std::make_unique<PipeWireBackend>());
  misuc::ui::MainWindow win(*engine);
  win.show();
  const int rc = app.exec();
  engine->closeAudio();
  return rc;
}