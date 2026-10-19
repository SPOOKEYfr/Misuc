#pragma once

#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QProgressBar>
#include <QTimer>

#include <misuc/engine/audio/AudioBackend.h>

namespace misuc::ui {

class RecordSetupDialog : public QDialog {
  Q_OBJECT
public:
  struct Config {
    enum class Type { Audio, Midi } type = Type::Audio;
    std::string inputDeviceId;
    std::string inputDeviceName;
    int inputChannels = 2;
    uint32_t sampleRate = 48000;
    std::string format = "wav"; // "wav", "flac"
    std::string recordPath; // project-relative or absolute
    std::string fileName;
    int countInBars = 2; // 0 = off, 1, 2
    bool metronomeOn = true;
    int destinationTrack = -1; // -1 = new track, >=0 = track index
    bool createNewTrack = true;
    bool inputMonitor = true;
  };

  explicit RecordSetupDialog(const std::vector<misuc::engine::AudioDeviceInfo>& inputDevices,
                             const QString& projectPath,
                             const std::vector<std::string>& trackNames,
                             int currentTrackIndex,
                             QWidget* parent = nullptr);

  Config getConfig() const { return config_; }
  bool isValid() const { return valid_; }

private:
  void setupUi();
  void populateDevices();
  void populateTracks();
  void updatePathDisplay();
  void onFormatChanged(int idx);
  void onBrowsePath();
  void validateAndAccept();

  Config config_;
  bool valid_ = false;

  std::vector<misuc::engine::AudioDeviceInfo> inputDevices_;
  QString projectPath_;
  std::vector<std::string> trackNames_;
  int currentTrackIndex_;

  // Widgets
  QButtonGroup* typeGroup_ = nullptr;
  QRadioButton* audioRadio_ = nullptr;
  QRadioButton* midiRadio_ = nullptr;

  QComboBox* deviceCombo_ = nullptr;
  QComboBox* channelsCombo_ = nullptr;
  QComboBox* sampleRateCombo_ = nullptr;

  QComboBox* formatCombo_ = nullptr;
  QLineEdit* pathEdit_ = nullptr;
  QPushButton* browseBtn_ = nullptr;
  QLineEdit* fileNameEdit_ = nullptr;

  QComboBox* countInCombo_ = nullptr;
  QCheckBox* metronomeCheck_ = nullptr;
  QCheckBox* monitorCheck_ = nullptr;

  QComboBox* destinationCombo_ = nullptr;
  QCheckBox* newTrackCheck_ = nullptr;

  // Input monitoring
  QProgressBar* inputMeterL_ = nullptr;
  QProgressBar* inputMeterR_ = nullptr;
  QTimer* meterTimer_ = nullptr;

  // Buttons
  QPushButton* okBtn_ = nullptr;
  QPushButton* cancelBtn_ = nullptr;
};

} // namespace misuc::ui