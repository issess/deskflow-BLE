/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ble/IBlePeripheralBackend.h"

#include <QLowEnergyService>
#include <QPointer>
#include <QTimer>

class QLowEnergyController;

namespace deskflow::ble {

// Qt Bluetooth-based peripheral. Used on Linux (BlueZ) and macOS. Also
// usable on Windows if the user's adapter happens to work with Qt's WinRT
// backend, but on Windows we default to WinRtBlePeripheralBackend.
class QtBlePeripheralBackend : public IBlePeripheralBackend
{
  Q_OBJECT
public:
  explicit QtBlePeripheralBackend(QObject *parent = nullptr);
  ~QtBlePeripheralBackend() override;

  bool start(const QString &localName, const QByteArray &manufacturerPayload) override;
  void stop() override;
  void sendPairingStatus(quint8 status) override;
  void sendDownstream(const QByteArray &chunk) override;
  int negotiatedMtu() const override
  {
    return m_mtu;
  }

private Q_SLOTS:
  void onServiceCharacteristicChanged(const QLowEnergyCharacteristic &ch, const QByteArray &value);
  void retryAdvertising();

private:
  void scheduleRetry();

  QPointer<QLowEnergyController> m_controller;
  QPointer<QLowEnergyService> m_service;
  QString m_localName;
  QByteArray m_mfgPayload;
  int m_mtu = 23;
  int m_advRetry = 0;
  QTimer *m_advRetryTimer = nullptr;
  bool m_started = false;
};

} // namespace deskflow::ble
