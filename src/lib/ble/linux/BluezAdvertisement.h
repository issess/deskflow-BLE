/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QString>
#include <QStringList>

namespace deskflow::ble::bluez {

// Owns a single LEAdvertisement1 DBus object exported under our chosen path
// (default `/org/deskflow/ble/adv0`). Used by BluezBlePeripheralBackend to
// hand BlueZ a description of the advert; BlueZ reads back our properties via
// `org.freedesktop.DBus.Properties.Get/GetAll` and copies them into the
// adapter's BLE adv PDU + scan response.
//
// Lifecycle:
//   1. Construct with the desired properties.
//   2. registerOnBus() — exports at the path on the system bus.
//   3. Hand the path to LEAdvertisingManager1.RegisterAdvertisement.
//   4. On teardown, call unregisterFromBus() AFTER calling
//      LEAdvertisingManager1.UnregisterAdvertisement (otherwise BlueZ races
//      against us reading back stale properties).
//
// If BlueZ itself decides to release the advert (controller reset, daemon
// restart) it sends a Release() method call. We surface that as the
// `released()` signal so the backend can rebuild from scratch.
class BluezAdvertisement : public QDBusVirtualObject
{
  Q_OBJECT
public:
  explicit BluezAdvertisement(QObject *parent = nullptr);
  ~BluezAdvertisement() override;

  // Mutators — call before registerOnBus().
  void setLocalName(const QString &name)
  {
    m_localName = name;
  }
  void setServiceUuids(const QStringList &uuids)
  {
    m_serviceUuids = uuids;
  }
  void setManufacturerData(quint16 companyId, const QByteArray &data);
  void setType(const QString &t)
  {
    m_type = t;
  }

  // Object path we register at (e.g. "/org/deskflow/ble/adv0").
  QDBusObjectPath path() const
  {
    return QDBusObjectPath(m_path);
  }
  void setPath(const QString &p)
  {
    m_path = p;
  }

  bool registerOnBus();
  void unregisterFromBus();

  // QDBusVirtualObject
  QString introspect(const QString &path) const override;
  bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;

Q_SIGNALS:
  // BlueZ has called Release() on us. The advert is gone; consumer should
  // tear down and restart if needed.
  void released();

private:
  QString m_path = QStringLiteral("/org/deskflow/ble/adv0");
  QString m_type = QStringLiteral("peripheral");
  QString m_localName;
  QStringList m_serviceUuids;
  quint16 m_mfgCompanyId = 0;
  QByteArray m_mfgData;
  bool m_registered = false;

  QVariantMap propertyMap() const;
};

} // namespace deskflow::ble::bluez
