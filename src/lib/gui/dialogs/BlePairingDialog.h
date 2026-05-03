/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QDialogButtonBox;

// Dual-mode BLE pairing dialog.
//
// Host mode (Deskflow running as server with transport=BLE): shows the
// 6-digit pairing code published by the running core. Closes on success /
// timeout / rejection.
//
// Remote mode (Deskflow running as client with transport=BLE): the user
// enters the 6-digit code displayed on the host. The dialog stores the
// code via BlePairingBroker so the next Client::connect() uses it, then
// waits for the pairing result.
class BlePairingDialog : public QDialog
{
  Q_OBJECT

public:
  enum class Mode
  {
    Host,
    Remote,
  };

  explicit BlePairingDialog(Mode mode, QWidget *parent = nullptr);
  ~BlePairingDialog() override;

Q_SIGNALS:
  // Emitted (remote mode) after the user has entered a valid 6-digit code
  // so the main window can kick off the client core without forcing the
  // user to dismiss the dialog and press Start separately.
  void remoteCodeSubmitted();

  // Emitted (host mode) when the user clicks "Regenerate code". The dialog
  // wipes the persisted PIN; the main window restarts the core so the new
  // run regenerates and re-publishes a fresh code.
  void regenerateRequested();

private Q_SLOTS:
  void onCodeChanged(const QString &code);
  void onPairingResult(bool accepted, const QString &reason);
  void onSubmitRemoteCode();
  void onRegenerateClicked();

private:
  void buildHostUi();
  void buildRemoteUi();
  static QString formatCode(const QString &raw);

  Mode m_mode;
  QStackedWidget *m_stack = nullptr;
  QLabel *m_hostCodeLabel = nullptr;
  QLabel *m_statusLabel = nullptr;
  QLineEdit *m_codeInput = nullptr;
  QPushButton *m_submitButton = nullptr;
  QPushButton *m_regenButton = nullptr;
  QDialogButtonBox *m_buttonBox = nullptr;
};
