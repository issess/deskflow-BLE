/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QMap>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>

// BlueZ exposes its object hierarchy through DBus ObjectManager. The signature
// of ObjectManager.GetManagedObjects() return value is a{oa{sa{sv}}}:
//   path -> { interface -> { property -> variant } }
//
// Qt's DBus marshalling needs the nested QMap aliases registered with the
// metatype system before they cross the wire in either direction. Centralising
// the typedefs and the registration call here keeps both peripheral and
// central backends in sync.

namespace deskflow::ble::bluez {

// a{sv}  — already QVariantMap, but alias for readability.
using PropertyMap = QVariantMap;

// a{sa{sv}} — interface name -> property map.
using InterfaceMap = QMap<QString, PropertyMap>;

// a{oa{sa{sv}}} — object path -> interface map. Returned by GetManagedObjects.
using ManagedObjects = QMap<QDBusObjectPath, InterfaceMap>;

// BlueZ's ManufacturerData is a{qv} — uint16 (company id) -> variant<ay>.
// QDBusVariant is required (rather than plain QVariant) because Qt's auto-
// generated map marshallers only emit/parse the DBus variant container when
// the value type is QDBusVariant.
using ManufacturerDataMap = QMap<quint16, QDBusVariant>;

// Idempotent registration. Safe to call from multiple backends; Qt guards
// duplicate metatype registrations internally. Call once per DBus connection
// before issuing any GetManagedObjects / parsing PropertiesChanged.
void registerBluezDbusTypes();

} // namespace deskflow::ble::bluez

Q_DECLARE_METATYPE(deskflow::ble::bluez::PropertyMap)
Q_DECLARE_METATYPE(deskflow::ble::bluez::InterfaceMap)
Q_DECLARE_METATYPE(deskflow::ble::bluez::ManagedObjects)
Q_DECLARE_METATYPE(deskflow::ble::bluez::ManufacturerDataMap)
