#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QToolBar>

#include "branding/Icons.h"

class QLabel;
class QToolButton;

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

class HintManager;
class MiniMeters;
class TransportBar;

// Global application header: brand cluster (mark + wordmark + project name) on
// the left, the transport centred, a workspace icon strip, and a live status
// cluster on the right (app CPU/RAM, engine rate, buffer+latency, xruns, audio
// state, master mini meters). Polled from the UI timer via refresh().
class HeaderBar : public QToolBar {
  Q_OBJECT
public:
  explicit HeaderBar(misuc::engine::AudioEngine& engine, QWidget* parent = nullptr);

  TransportBar* transportBar() const { return transport_; }
  QLabel* navLogo() const { return navLogo_; }

  // Registers the workspace-strip icons and status cluster in the global
  // HintBar (§78) so every control is discoverable on hover.
  void attachHints(HintManager* hints);

  void refresh();                  // status + meters + cpu/ram
  void setProjectName(const QString& name);

  // Syncs the workspace-strip pressed state after docks open/close.
  void setWorkspaceActive(const QString& workspaceId, bool active);
  // Reflects snap / song-pattern state on the strip's transport buttons.
  void setSnapActive(bool on);
  void setSongMode(bool song);

signals:
  // A workspace-strip icon was clicked; MainWindow routes it to the workspace.
  void workspaceIconClicked(const QString& workspaceId);
  // The logo was clicked; MainWindow opens the project setup page.
  void projectRequested();

private:
  bool eventFilter(QObject* watched, QEvent* event) override;
  QWidget* buildLeftCluster();
  QWidget* buildCenterTransport();
  QWidget* buildWorkspaceStrip();
  QWidget* buildRightCluster();
  void refreshCpuRam();
  void refreshLogo();
  QToolButton* stripButton(const QString& id, icons::IconId icon);
  void syncStripIcons();
  // Applies the status-cluster label colors only when they actually change
  // (state transitions / theme changes), so the 30 Hz UI poll never re-polishes
  // any QSS — a genuine repaint-storm root cause during live resize.
  void syncStatusColors();

  misuc::engine::AudioEngine& engine_;
  QLabel* navLogo_ = nullptr;
  QLabel* wordmark_ = nullptr;
  QLabel* projectName_ = nullptr;
  TransportBar* transport_ = nullptr;
  QHash<QString, QToolButton*> wsButtons_;
  QHash<QString, icons::IconId> wsIconIds_;
  QLabel* cpuLabel_ = nullptr;
  QLabel* ramLabel_ = nullptr;
  QLabel* srLabel_ = nullptr;
  QLabel* latencyLabel_ = nullptr;
  QLabel* xrunLabel_ = nullptr;
  QLabel* stateLabel_ = nullptr;
  MiniMeters* meters_ = nullptr;
  long clockTicks_ = 100;
  QElapsedTimer cpuTimer_;
  uint64_t cpuLastJiffies_ = 0;
  QHash<const QLabel*, QColor> statusColorCache_;
};

} // namespace misuc::ui