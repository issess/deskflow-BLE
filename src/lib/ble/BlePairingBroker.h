/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QString>

namespace deskflow::ble {

// Process-wide channel between the BLE listener and any UI that wants to
// display the current pairing code. The listener calls `setActiveCode()` /
// `clearActiveCode()`; dialogs connect to the signals.
//
// Thread-safe: all access is serialized through the broker's thread
// affinity (main Qt thread). Listener callbacks already run on that thread
// because they come from QLowEnergyService signals.
class BlePairingBroker : public QObject
{
  Q_OBJECT
public:
  static BlePairingBroker &instance();

  void setActiveCode(const QString &code);
  void clearActiveCode();
  QString activeCode() const
  {
    return m_code;
  }

  // Remote/central side: the user enters a code into the pairing dialog;
  // the dialog calls setPendingCode() so the BleSocket can consume it
  // during connect(). Cleared once connect completes or fails.
  void setPendingCode(const QString &code)
  {
    m_pendingCode = code;
  }
  void clearPendingCode()
  {
    m_pendingCode.fill(QChar(u'\0'));
    m_pendingCode.clear();
  }
  QString pendingCode() const
  {
    return m_pendingCode;
  }

  // Reports a pairing attempt result so the UI can show success/failure and
  // close itself.
  void reportResult(bool accepted, const QString &reason = {});

Q_SIGNALS:
  void codeChanged(const QString &code); // empty string means cleared
  void pairingResult(bool accepted, const QString &reason);

private:
  BlePairingBroker() = default;
  QString m_code;
  QString m_pendingCode;
};

} // namespace deskflow::ble
