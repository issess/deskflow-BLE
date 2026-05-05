/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ble/linux/BluezDbusTypes.h"

#include <QDBusObjectPath>
#include <QObject>
#include <QString>

namespace deskflow::ble::bluez {

// Picks the first powered org.bluez.Adapter1 from a snapshot of the BlueZ
// ObjectManager tree. Pure function — testable without a live system bus.
//
// Returns an empty QDBusObjectPath if no adapter is powered (or no adapter
// exists at all). When multiple adapters are powered, returns the
// lexicographically smallest path so the choice is stable across runs and
// matches what `QBluetoothLocalDevice::allDevices().first()` picks today.
QDBusObjectPath pickAdapter(const ManagedObjects &objects);

// Owns the lifecycle of "which BlueZ adapter are we using."
//
// On start():
//   1. Confirms `org.bluez` is owned on the system bus.
//   2. Calls GetManagedObjects, picks the first powered Adapter1.
//   3. Subscribes to PropertiesChanged on that adapter to surface power off
//      as `poweredChanged(false)`.
//   4. Subscribes to NameOwnerChanged for org.bluez to surface daemon
//      restart as `bluezDisappeared()`.
//
// All signals are emitted on the thread that called start() (main thread,
// per BlePeripheralContext::moveToThread in BleListenSocket.cpp:336).
class BluezAdapter : public QObject
{
  Q_OBJECT
public:
  explicit BluezAdapter(QObject *parent = nullptr);
  ~BluezAdapter() override;

  // Returns true if an adapter was found and powered. On false, `errorOut`
  // is filled with a user-readable diagnostic ("BlueZ not running",
  // "no Bluetooth adapter", "Bluetooth is off").
  bool start(QString *errorOut = nullptr);

  // Stop watching. Safe to call multiple times.
  void stop();

  // The DBus object path of the selected adapter, e.g. "/org/bluez/hci0".
  // Empty until start() returns true.
  QDBusObjectPath path() const
  {
    return m_path;
  }

  // The BlueZ daemon version reported by AgentManager1.AgentManager interface
  // (or 0 if unknown). Used for capability gating (e.g. ConnectDevice needs
  // 5.49+, MTU property needs 5.62+). Captured during start().
  int bluezMajor() const
  {
    return m_bluezMajor;
  }
  int bluezMinor() const
  {
    return m_bluezMinor;
  }

Q_SIGNALS:
  void poweredChanged(bool powered);
  void bluezDisappeared();

private:
  void handlePropertiesChanged(const QString &interface, const QVariantMap &changed,
                               const QStringList &invalidated);
  void handleNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);
  void probeBluezVersion();

  QDBusObjectPath m_path;
  bool m_running = false;
  int m_bluezMajor = 0;
  int m_bluezMinor = 0;
};

} // namespace deskflow::ble::bluez
