/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BlePairingDialog.h"

#include "ble/BlePairingBroker.h"
#include "common/Settings.h"

#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QStackedWidget>
#include <QVBoxLayout>

BlePairingDialog::BlePairingDialog(Mode mode, QWidget *parent) : QDialog(parent), m_mode(mode)
{
  setWindowTitle(mode == Mode::Host ? tr("BLE Pairing (Host)") : tr("BLE Pairing (Remote)"));
  // Non-modal so the user can still start/stop the core from MainWindow
  // while the dialog observes the pairing state.
  setModal(false);
  setWindowModality(Qt::NonModal);
  resize(420, 240);

  auto *layout = new QVBoxLayout(this);

  if (mode == Mode::Host) {
    buildHostUi();
  } else {
    buildRemoteUi();
  }

  m_statusLabel = new QLabel(this);
  m_statusLabel->setWordWrap(true);
  m_statusLabel->setAlignment(Qt::AlignCenter);
  layout->addWidget(m_statusLabel);

  m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
  layout->addWidget(m_buttonBox);
  connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto &broker = deskflow::ble::BlePairingBroker::instance();
  connect(&broker, &deskflow::ble::BlePairingBroker::codeChanged, this, &BlePairingDialog::onCodeChanged);
  connect(&broker, &deskflow::ble::BlePairingBroker::pairingResult, this, &BlePairingDialog::onPairingResult);

  // Prime host display with any code already active.
  if (mode == Mode::Host)
    onCodeChanged(broker.activeCode());
}

BlePairingDialog::~BlePairingDialog()
{
  // Intentionally do NOT clear Settings::Client::PendingBleCode here. The
  // core consumes it inside BleSocket::connect; clearing on dialog close
  // would race with core startup and wipe the value before it is read.
  // The broker clear is safe because it's GUI-process-local.
  if (m_mode == Mode::Remote)
    deskflow::ble::BlePairingBroker::instance().clearPendingCode();
}

void BlePairingDialog::buildHostUi()
{
  auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
  auto *title = new QLabel(tr("Enter this code on the remote device:"), this);
  title->setAlignment(Qt::AlignCenter);
  layout->addWidget(title);

  m_hostCodeLabel = new QLabel(QStringLiteral("——— ———"), this);
  QFont f = m_hostCodeLabel->font();
  f.setFamily(QStringLiteral("Consolas"));
  f.setStyleHint(QFont::Monospace);
  f.setPointSize(44);
  f.setBold(true);
  f.setLetterSpacing(QFont::AbsoluteSpacing, 6);
  m_hostCodeLabel->setFont(f);
  m_hostCodeLabel->setAlignment(Qt::AlignCenter);
  layout->addWidget(m_hostCodeLabel);

  auto *hint = new QLabel(
      tr("The code resets every pairing session and is discarded once\n"
         "the remote connects. Keep this window open until paired."),
      this
  );
  hint->setAlignment(Qt::AlignCenter);
  layout->addWidget(hint);

  // Ensure transport is set to BLE so the running core (if user starts it
  // now) uses the BLE listener path.
  Settings::setValue(Settings::Core::Transport, QStringLiteral("ble"));
  Settings::save();
}

void BlePairingDialog::buildRemoteUi()
{
  auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
  auto *title = new QLabel(tr("Enter the 6-digit code shown on the host:"), this);
  title->setAlignment(Qt::AlignCenter);
  layout->addWidget(title);

  auto *inputRow = new QHBoxLayout();
  inputRow->addStretch();
  m_codeInput = new QLineEdit(this);
  m_codeInput->setMaxLength(6);
  m_codeInput->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("^\\d{6}$")), this));
  m_codeInput->setAlignment(Qt::AlignCenter);
  QFont f = m_codeInput->font();
  f.setFamily(QStringLiteral("Consolas"));
  f.setStyleHint(QFont::Monospace);
  f.setPointSize(24);
  m_codeInput->setFont(f);
  m_codeInput->setFixedWidth(200);
  inputRow->addWidget(m_codeInput);
  inputRow->addStretch();
  layout->addLayout(inputRow);

  m_submitButton = new QPushButton(tr("Pair"), this);
  m_submitButton->setDefault(true);
  connect(m_submitButton, &QPushButton::clicked, this, &BlePairingDialog::onSubmitRemoteCode);
  connect(m_codeInput, &QLineEdit::returnPressed, this, &BlePairingDialog::onSubmitRemoteCode);
  auto *btnRow = new QHBoxLayout();
  btnRow->addStretch();
  btnRow->addWidget(m_submitButton);
  btnRow->addStretch();
  layout->addLayout(btnRow);

  Settings::setValue(Settings::Core::Transport, QStringLiteral("ble"));
  Settings::save();
}

void BlePairingDialog::onCodeChanged(const QString &code)
{
  if (m_mode != Mode::Host || !m_hostCodeLabel)
    return;
  if (code.isEmpty()) {
    m_hostCodeLabel->setText(QStringLiteral("——— ———"));
    if (m_statusLabel)
      m_statusLabel->setText(tr("Waiting for core to start advertising…"));
  } else {
    m_hostCodeLabel->setText(formatCode(code));
    if (m_statusLabel)
      m_statusLabel->setText(tr("Advertising — awaiting remote"));
  }
}

void BlePairingDialog::onPairingResult(bool accepted, const QString &reason)
{
  if (accepted) {
    if (m_statusLabel)
      m_statusLabel->setText(tr("Paired successfully."));
    if (m_submitButton)
      m_submitButton->setEnabled(false);
    QMetaObject::invokeMethod(this, &QDialog::accept, Qt::QueuedConnection);
  } else {
    if (m_statusLabel)
      m_statusLabel->setText(tr("Pairing failed: %1").arg(reason.isEmpty() ? tr("unknown error") : reason));
    if (m_submitButton)
      m_submitButton->setEnabled(true);
    if (m_codeInput) {
      m_codeInput->clear();
      m_codeInput->setFocus();
    }
  }
}

void BlePairingDialog::onSubmitRemoteCode()
{
  if (!m_codeInput)
    return;
  const QString code = m_codeInput->text();
  if (code.size() != 6) {
    if (m_statusLabel)
      m_statusLabel->setText(tr("Enter all 6 digits."));
    return;
  }
  // Store in Settings so the core process (deskflow-core.exe) sees it; the
  // GUI-side broker is process-local and cannot reach the core. The core
  // clears this key after consuming it.
  Settings::setValue(Settings::Client::PendingBleCode, code);
  Settings::save();
  const QString verify = Settings::value(Settings::Client::PendingBleCode).toString();
  qInfo("BLE pairing: code persisted to Settings, read-back length=%d file=%s",
        verify.size(), qUtf8Printable(Settings::settingsFile()));
  deskflow::ble::BlePairingBroker::instance().setPendingCode(code);
  if (m_statusLabel)
    m_statusLabel->setText(tr("Code submitted. Starting client core…"));
  if (m_submitButton)
    m_submitButton->setEnabled(false);
  Q_EMIT remoteCodeSubmitted();
}

QString BlePairingDialog::formatCode(const QString &raw)
{
  if (raw.size() != 6)
    return raw;
  return raw.left(3) + QStringLiteral("  ") + raw.mid(3);
}
