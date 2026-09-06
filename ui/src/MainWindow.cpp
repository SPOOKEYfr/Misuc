#include "MainWindow.h"

#include <memory>
#include <utility>

#include <QApplication>
#include <QButtonGroup>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QPainter>

#include <misuc/engine/audio/AudioEngine.h>

#include "AudioSettingsPage.h"
#include "ProjectPage.h"
#include "StatusPanel.h"
#include "Theme.h"
#include "TransportBar.h"

namespace misuc::ui {

MainWindow::MainWindow(misuc::engine::AudioEngine& engine) : engine_(engine) {
  setWindowTitle(QStringLiteral("Misuc"));
  resize(1180, 760);
  setMinimumSize(960, 620);

  auto* central = new QWidget(this);
  central->setObjectName(QStringLiteral("central"));
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // Top transport bar.
  transport_ = new TransportBar(engine_, this);
  root->addWidget(transport_);

  // Middle: nav rail + page stack.
  auto* middle = new QWidget(central);
  auto* midLay = new QHBoxLayout(middle);
  midLay->setContentsMargins(0, 0, 0, 0);
  midLay->setSpacing(0);

  navRail_ = new QWidget(middle);
  navRail_->setFixedWidth(150);
  auto* navLay = new QVBoxLayout(navRail_);
  navLay->setContentsMargins(10, 18, 10, 18);
  navLay->setSpacing(4);

  auto* navGroup = new QButtonGroup(this);
  navGroup->setExclusive(true);

  auto addNav = [this, navLay, navGroup](const QString& label, int page) {
    auto* btn = new QPushButton(label, navRail_);
    btn->setCheckable(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setProperty("navPage", page);
    btn->setObjectName(QStringLiteral("navButton"));
    if (navGroup->buttons().isEmpty())
      btn->setChecked(true);
    navGroup->addButton(btn, page);
    navLay->addWidget(btn);
    connect(btn, &QPushButton::clicked, this, [this, page] {
      pages_->setCurrentIndex(page);
    });
  };

  addNav(QStringLiteral("Project"), 0);
  addNav(QStringLiteral("Audio"), 1);
  navLay->addStretch();

  midLay->addWidget(navRail_);
  pages_ = new QStackedWidget(middle);
  midLay->addWidget(pages_, 1);
  root->addWidget(middle, 1);

  // Status strip.
  status_ = new StatusPanel(engine_, this);
  root->addWidget(status_);

  setCentralWidget(central);

  buildPages();
  pages_->setCurrentIndex(0);

  // Poll transport + status at 30 Hz.
  timer_ = new QTimer(this);
  timer_->setInterval(33);
  connect(timer_, &QTimer::timeout, this, [this] { poll(); });
  timer_->start();
  poll();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildPages() {
  pages_->addWidget(new ProjectPage(engine_, this));
  pages_->addWidget(new AudioSettingsPage(engine_, this));
}

void MainWindow::poll() {
  transport_->refresh();
  status_->refresh();
}

void MainWindow::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), theme::kBackground);
}

} // namespace misuc::ui