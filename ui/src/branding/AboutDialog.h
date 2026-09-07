#pragma once

#include <QDialog>

namespace misuc::ui {

// Help -> About Misuc: brand mark, version, the author's contact, and their
// photo, all themed from the current token set.
class AboutDialog : public QDialog {
public:
  explicit AboutDialog(QWidget* parent = nullptr);

private:
  void buildUi();
};

} // namespace misuc::ui