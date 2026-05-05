/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ble/linux/BluezDbusTypes.h"

#include <QByteArray>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QBluetoothUuid;

namespace deskflow::ble::bluez {

// Owns the GATT application object tree under a chosen root path
// (default `/org/deskflow/ble`). One QDBusVirtualObject handles every path
// under the root via SubPath registration:
//
//   /org/deskflow/ble                  → ObjectManager
//   /org/deskflow/ble/service0         → org.bluez.GattService1
//   /org/deskflow/ble/service0/charN   → org.bluez.GattCharacteristic1
//
// Lifecycle:
//   1. Construct with the service UUID.
//   2. addCharacteristic(...) for each char (UUID + flags).
//   3. setRootPath(...) (optional, defaults to /org/deskflow/ble).
//   4. registerOnBus() — exports the tree.
//   5. Hand the root path to GattManager1.RegisterApplication.
//   6. After UnregisterApplication, call unregisterFromBus().
//
// Inbound write notifications are surfaced as `characteristicWritten`.
// Outbound notifies are sent by setting the cached Value via setValue() and
// emitting PropertiesChanged through this object.
class BluezGattApplication : public QDBusVirtualObject
{
  Q_OBJECT
public:
  enum class CharFlag : uint
  {
    Read = 1u << 0,
    Write = 1u << 1,
    WriteWithoutResponse = 1u << 2,
    Notify = 1u << 3,
    Indicate = 1u << 4,
  };
  Q_DECLARE_FLAGS(CharFlags, CharFlag)

  explicit BluezGattApplication(QObject *parent = nullptr);
  ~BluezGattApplication() override;

  void setRootPath(const QString &p)
  {
    m_root = p;
  }
  QDBusObjectPath rootPath() const
  {
    return QDBusObjectPath(m_root);
  }

  void setServiceUuid(const QString &u)
  {
    m_serviceUuid = u;
  }

  // Register a characteristic. UUID is the canonical lowercase form. `flags`
  // is a bitmask of CharFlag values. The path under the service is auto-
  // generated as charN. Call before registerOnBus(); calls after register
  // are ignored.
  void addCharacteristic(const QString &uuid, CharFlags flags);

  bool registerOnBus();
  void unregisterFromBus();

  // Update a characteristic's cached Value. If `notify` is true and the char
  // has notify subscribers, emits PropertiesChanged so BlueZ forwards the
  // value to the subscriber. Lookup is by UUID (case-insensitive).
  // Returns false if no such characteristic was registered.
  bool setValue(const QString &uuid, const QByteArray &value, bool notify);

  // Number of characteristics currently in `notifying` state (any char).
  int subscriberCount() const;

  // Most recently observed ATT MTU (from WriteValue options on any char), or 0.
  int observedMtu() const
  {
    return m_observedMtu;
  }

  // QDBusVirtualObject
  QString introspect(const QString &path) const override;
  bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;

Q_SIGNALS:
  // BlueZ delivered a WriteValue on a characteristic. `device` is the central's
  // object path (may be empty on older BlueZ).
  void characteristicWritten(const QString &uuid, const QByteArray &value, const QString &device);

  // Aggregate subscribe count changed (sum of `Notifying` across our chars).
  void subscriberCountChanged(int count);

  // Observed MTU has changed (first-time or upgrade).
  void mtuChanged(int mtu);

private:
  struct CharState
  {
    QString uuid;
    QString path; // full DBus object path
    CharFlags flags;
    QByteArray value;
    bool notifying = false;
  };

  // Path roles for handleMessage dispatch.
  enum class PathRole
  {
    Unknown,
    Root,
    Service,
    Characteristic,
  };
  struct PathLookup
  {
    PathRole role = PathRole::Unknown;
    int charIndex = -1; // valid when role == Characteristic
  };
  PathLookup classifyPath(const QString &path) const;

  bool handleObjectManagerGetManagedObjects(const QDBusMessage &m, const QDBusConnection &c);
  bool handleProperties(const QDBusMessage &m, const QDBusConnection &c, const PathLookup &lookup);
  bool handleService(const QDBusMessage &m, const QDBusConnection &c);
  bool handleCharacteristic(const QDBusMessage &m, const QDBusConnection &c, int charIndex);

  QVariantMap servicePropertyMap() const;
  QVariantMap characteristicPropertyMap(int charIndex) const;
  QStringList flagStrings(CharFlags f) const;

  void emitValueChanged(int charIndex);

  QString m_root = QStringLiteral("/org/deskflow/ble");
  QString m_serviceUuid;
  QList<CharState> m_chars;
  bool m_registered = false;
  int m_observedMtu = 0;
};

} // namespace deskflow::ble::bluez

Q_DECLARE_OPERATORS_FOR_FLAGS(deskflow::ble::bluez::BluezGattApplication::CharFlags)
