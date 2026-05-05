/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ble/IBlePeripheralBackend.h"
#include "ble/linux/BluezAdapter.h"
#include "ble/linux/BluezAdvertisement.h"
#include "ble/linux/BluezGattApplication.h"

#include <QByteArray>
#include <QDBusError>
#include <QObject>
#include <QString>

class QDBusPendingCallWatcher;

namespace deskflow::ble {

// Linux peripheral backend speaking org.bluez directly via Qt6::DBus. Exposes
// the same Deskflow GATT service as QtBlePeripheralBackend but bypasses Qt's
// QLowEnergyController, sidestepping its known issues with advertising on
// Broadcom/Realtek adapters and missing MTU events.
class BluezBlePeripheralBackend : public IBlePeripheralBackend
{
  Q_OBJECT
public:
  explicit BluezBlePeripheralBackend(QObject *parent = nullptr);
  ~BluezBlePeripheralBackend() override;

  bool start(const QString &localName, const QByteArray &manufacturerPayload) override;
  void stop() override;
  void sendPairingStatus(quint8 status) override;
  void sendDownstream(const QByteArray &chunk) override;
  int negotiatedMtu() const override
  {
    return m_mtu;
  }

private:
  void onAdvertisementRegistered(QDBusPendingCallWatcher *watcher);
  void onApplicationRegistered(QDBusPendingCallWatcher *watcher);
  void onCharacteristicWritten(const QString &uuid, const QByteArray &value, const QString &device);
  void onSubscriberCountChanged(int count);
  void onMtuChanged(int mtu);
  void onAdvertisementReleased();
  void onAdapterPoweredChanged(bool powered);
  void onBluezDisappeared();

  void emitFatal(const QString &reason);
  void buildGattApplication();

  bluez::BluezAdapter *m_adapter = nullptr;
  bluez::BluezAdvertisement *m_adv = nullptr;
  bluez::BluezGattApplication *m_app = nullptr;

  bool m_started = false;
  int m_mtu = 64;
  int m_lastSubscriberCount = 0;
  bool m_advertRegistered = false;
  bool m_appRegistered = false;
  QString m_localName;
  QByteArray m_mfgPayload;
};

} // namespace deskflow::ble
