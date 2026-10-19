#include "export/ExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include "Theme.h"

namespace misuc::ui {

namespace {
// Human-readable label for each export container, mirroring the probed order.
QString formatLabel(misuc::audio::ExportFormat f) {
  switch (f) {
    case misuc::audio::ExportFormat::WavFloat:
      return QStringLiteral("WAV         32-bit float (lossless)");
    case misuc::audio::ExportFormat::WavPcm16:
      return QStringLiteral("WAV         16-bit PCM");
    case misuc::audio::ExportFormat::Flac:
      return QStringLiteral("FLAC        16-bit lossless");
    case misuc::audio::ExportFormat::Mp3:
      return QStringLiteral("MP3         Layer III (lossy)");
    case misuc::audio::ExportFormat::OggVorbis:
      return QStringLiteral("Ogg Vorbis  (lossy)");
    case misuc::audio::ExportFormat::Aiff:
      return QStringLiteral("AIFF        16-bit PCM");
  }
  return QStringLiteral("WAV");
}
} // namespace

ExportDialog::ExportDialog(misuc::audio::ExportFormat initialFormat,
                           bool loopAvailable, bool selectionAvailable,
                           QWidget* parent, const QString& title,
                           const QString& artist, uint32_t sampleRate)
    : QDialog(parent) {
  setWindowTitle(QStringLiteral("Export audio"));
  setModal(true);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 12);
  root->setSpacing(14);

  auto* head = new QLabel(QStringLiteral("Bounce the mix to an audio file."));
  head->setStyleSheet(QStringLiteral("color: %1; font-size: 10pt;")
                          .arg(theme::kTextDim.name()));
  root->addWidget(head);

  auto* form = new QFormLayout;
  form->setSpacing(10);

  format_ = new QComboBox;
  format_->setObjectName(QStringLiteral("exportFormatCombo"));
  // Probe the codecs this libsndfile build can actually write; WAV always
  // appears first. Unsupported codecs are never offered.
  const std::vector<misuc::audio::ExportFormat> kFormats =
      misuc::audio::availableExportFormats();
  int initialIdx = 0;
  for (size_t i = 0; i < kFormats.size(); ++i) {
    format_->addItem(formatLabel(kFormats[i]),
                     QVariant::fromValue(static_cast<int>(kFormats[i])));
    if (kFormats[i] == initialFormat)
      initialIdx = static_cast<int>(i);
  }
  format_->setCurrentIndex(initialIdx);
  connect(format_, &QComboBox::currentIndexChanged, this,
          &ExportDialog::onFormatChanged);
  form->addRow(QStringLiteral("Format"), format_);

  range_ = new QComboBox;
  range_->setObjectName(QStringLiteral("exportRangeCombo"));
  range_->addItem(QStringLiteral("Whole song"),
                  QVariant::fromValue(static_cast<int>(ExportRange::WholeSong)));
  range_->addItem(QStringLiteral("Loop region"),
                  QVariant::fromValue(static_cast<int>(ExportRange::Loop)));
  range_->addItem(QStringLiteral("Selection"),
                  QVariant::fromValue(static_cast<int>(ExportRange::Selection)));
  auto* rangeModel = qobject_cast<QStandardItemModel*>(range_->model());
  if (rangeModel) {
    if (rangeModel->item(1))
      rangeModel->item(1)->setEnabled(loopAvailable);
    if (rangeModel->item(2))
      rangeModel->item(2)->setEnabled(selectionAvailable);
  }
  form->addRow(QStringLiteral("Range"), range_);

  title_ = new QLineEdit(title, this);
  title_->setObjectName(QStringLiteral("exportTitleEdit"));
  form->addRow(QStringLiteral("Title"), title_);

  artist_ = new QLineEdit(artist, this);
  artist_->setObjectName(QStringLiteral("exportArtistEdit"));
  form->addRow(QStringLiteral("Artist"), artist_);

  sampleRate_ = new QComboBox;
  sampleRate_->setObjectName(QStringLiteral("exportRateCombo"));
  const std::vector<uint32_t> kRates = {48000, 44100, 96000, 192000};
  for (const uint32_t r : kRates) {
    sampleRate_->addItem(QStringLiteral("%1 Hz").arg(r),
                         QVariant::fromValue(static_cast<int>(r)));
  }
  int rateIdx = sampleRate_->findData(
      static_cast<int>(sampleRate != 0 ? sampleRate : 48000));
  sampleRate_->setCurrentIndex(rateIdx < 0 ? 0 : rateIdx);
  form->addRow(QStringLiteral("Sample rate"), sampleRate_);

  dither_ = new QCheckBox(
      QStringLiteral("Apply dither (fixed-word targets: decorrelates rounding error)"));
  dither_->setObjectName(QStringLiteral("exportDitherCheck"));
  dither_->setChecked(true);
  form->addRow(QString(), dither_);

  metronome_ = new QCheckBox(
      QStringLiteral("Include metronome in the export"));
  metronome_->setObjectName(QStringLiteral("exportMetronomeCheck"));
  metronome_->setChecked(false);
  form->addRow(QString(), metronome_);

  root->addLayout(form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                       QDialogButtonBox::Cancel);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Export"));
  root->addWidget(buttons);

  // Remember last-used export settings (protocol #13 §14) so a repeat export
  // starts from the previous setup. Project-specific fields (title/artist) are
  // not persisted -- they come from the session.
  QSettings s(QStringLiteral("misuc"), QStringLiteral("misuc"));
  if (s.contains(QStringLiteral("export/format"))) {
    const int last = s.value(QStringLiteral("export/format")).toInt();
    const int idx = format_->findData(last);
    if (idx >= 0)
      format_->setCurrentIndex(idx);
  }
  if (s.contains(QStringLiteral("export/range"))) {
    const int last = s.value(QStringLiteral("export/range")).toInt();
    const int idx = range_->findData(last);
    if (idx >= 0)
      range_->setCurrentIndex(idx);
  }
  if (s.contains(QStringLiteral("export/dither")))
    dither_->setChecked(s.value(QStringLiteral("export/dither")).toBool());
  if (s.contains(QStringLiteral("export/metronome")))
    metronome_->setChecked(
        s.value(QStringLiteral("export/metronome")).toBool());
  if (s.contains(QStringLiteral("export/sampleRate"))) {
    const int last = s.value(QStringLiteral("export/sampleRate")).toInt();
    const int idx = sampleRate_->findData(last);
    if (idx >= 0)
      sampleRate_->setCurrentIndex(idx);
  }

  setStyleSheet(QStringLiteral(
                    "QDialog { background: %1; }"
                    "QComboBox, QCheckBox { color: %2; }"
                    "QComboBox QAbstractItemView { background: %3; color: %2; }"
                    "QPushButton { color: %2; }")
                    .arg(theme::kPanel.name(), theme::kText.name(),
                         theme::kPanelRaised.name()));
  onFormatChanged();
  setFixedWidth(460);
}

misuc::audio::ExportFormat ExportDialog::format() const {
  return static_cast<misuc::audio::ExportFormat>(
      format_->currentData().toInt());
}

ExportRange ExportDialog::range() const {
  const int v = range_->currentData().toInt();
  if (v == static_cast<int>(ExportRange::Loop))
    return ExportRange::Loop;
  if (v == static_cast<int>(ExportRange::Selection))
    return ExportRange::Selection;
  return ExportRange::WholeSong;
}

bool ExportDialog::dither() const { return dither_->isChecked(); }

bool ExportDialog::includeMetronome() const {
  return metronome_->isChecked();
}

QString ExportDialog::title() const { return title_->text().trimmed(); }
QString ExportDialog::artist() const { return artist_->text().trimmed(); }

uint32_t ExportDialog::sampleRate() const {
  const int v = sampleRate_->currentData().toInt();
  return v > 0 ? static_cast<uint32_t>(v) : 0;
}

QString ExportDialog::suffix() const {
  return QString::fromLatin1(
      misuc::audio::exportFormatSuffix(format()));
}

void ExportDialog::onFormatChanged() {
  // Dither only exists for fixed-word-length targets (PCM/FLAC/AIFF); lossy
  // codecs (MP3/OGG) get the raw float mix.
  const auto f = format();
  dither_->setEnabled(f != misuc::audio::ExportFormat::WavFloat &&
                      f != misuc::audio::ExportFormat::Mp3 &&
                      f != misuc::audio::ExportFormat::OggVorbis);
  // Remember the pick immediately so an accept that never comes still records
  // the user's format/range/rate preference.
  QSettings s(QStringLiteral("misuc"), QStringLiteral("misuc"));
  s.setValue(QStringLiteral("export/format"),
             static_cast<int>(format_ == nullptr
                                  ? static_cast<int>(misuc::audio::ExportFormat::WavFloat)
                                  : format_->currentData().toInt()));
  s.setValue(QStringLiteral("export/range"),
             range_ ? range_->currentData().toInt()
                    : static_cast<int>(ExportRange::WholeSong));
  s.setValue(QStringLiteral("export/dither"), dither_->isChecked());
  s.setValue(QStringLiteral("export/metronome"), metronome_->isChecked());
  if (sampleRate_)
    s.setValue(QStringLiteral("export/sampleRate"),
               sampleRate_->currentData().toInt());
}

} // namespace misuc::ui