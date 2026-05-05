/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/linux/BluezAdvertisement.h"

#include "base/Log.h"
#include "ble/linux/BluezDbusTypes.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QVariant>

namespace deskflow::ble::bluez {

namespace {

constexpr const char *kAdvertisement1 = "org.bluez.LEAdvertisement1";
constexpr const char *kProperties = "org.freedesktop.DBus.Properties";
constexpr const char *kIntrospectable = "org.freedesktop.DBus.Introspectable";

const QString kIntrospectXml = QStringLiteral(
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\""
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.bluez.LEAdvertisement1\">\n"
    "    <method name=\"Release\"/>\n"
    "    <property name=\"Type\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"ServiceUUIDs\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"ManufacturerData\" type=\"a{qv}\" access=\"read\"/>\n"
    "    <property name=\"LocalName\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Discoverable\" type=\"b\" access=\"read\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "</node>\n");

} // namespace

BluezAdvertisement::BluezAdvertisement(QObject *parent) : QDBusVirtualObject(parent)
{
  registerBluezDbusTypes();
}

BluezAdvertisement::~BluezAdvertisement()
{
  unregisterFromBus();
}

void BluezAdvertisement::setManufacturerData(quint16 companyId, const QByteArray &data)
{
  m_mfgCompanyId = companyId;
  m_mfgData = data;
}

bool BluezAdvertisement::registerOnBus()
{
  if (m_registered)
    return true;
  auto bus = QDBusConnection::systemBus();
  if (!bus.registerVirtualObject(m_path, this, QDBusConnection::SubPath)) {
    LOG_ERR("BluezAdvertisement: registerVirtualObject(%s) failed: %s",
            m_path.toUtf8().constData(), bus.lastError().message().toUtf8().constData());
    return false;
  }
  m_registered = true;
  return true;
}

void BluezAdvertisement::unregisterFromBus()
{
  if (!m_registered)
    return;
  m_registered = false;
  auto bus = QDBusConnection::systemBus();
  bus.unregisterObject(m_path);
}

QString BluezAdvertisement::introspect(const QString &) const
{
  return kIntrospectXml;
}

QVariantMap BluezAdvertisement::propertyMap() const
{
  QVariantMap props;
  props.insert(QStringLiteral("Type"), m_type);
  props.insert(QStringLiteral("ServiceUUIDs"), m_serviceUuids);
  props.insert(QStringLiteral("LocalName"), m_localName);
  props.insert(QStringLiteral("Discoverable"), true);

  // ManufacturerData is a{qv} — qDBusRegisterMetaType registered the alias.
  ManufacturerDataMap mfg;
  mfg.insert(m_mfgCompanyId, QDBusVariant(QVariant::fromValue(m_mfgData)));
  props.insert(QStringLiteral("ManufacturerData"), QVariant::fromValue(mfg));

  return props;
}

bool BluezAdvertisement::handleMessage(const QDBusMessage &message, const QDBusConnection &connection)
{
  const QString iface = message.interface();
  const QString member = message.member();

  if (iface == QString::fromLatin1(kIntrospectable) && member == QStringLiteral("Introspect")) {
    auto reply = message.createReply(QVariant(kIntrospectXml));
    return connection.send(reply);
  }

  if (iface == QString::fromLatin1(kAdvertisement1) && member == QStringLiteral("Release")) {
    LOG_NOTE("BluezAdvertisement: Release() called by BlueZ");
    auto reply = message.createReply();
    connection.send(reply);
    Q_EMIT released();
    return true;
  }

  if (iface == QString::fromLatin1(kProperties)) {
    if (member == QStringLiteral("GetAll")) {
      const QString requestedIface = message.arguments().value(0).toString();
      if (requestedIface != QString::fromLatin1(kAdvertisement1)) {
        auto err = message.createErrorReply(QDBusError::UnknownInterface,
                                            QStringLiteral("Unknown interface: %1").arg(requestedIface));
        return connection.send(err);
      }
      auto reply = message.createReply(QVariant::fromValue(propertyMap()));
      return connection.send(reply);
    }
    if (member == QStringLiteral("Get")) {
      const QString requestedIface = message.arguments().value(0).toString();
      const QString prop = message.arguments().value(1).toString();
      if (requestedIface != QString::fromLatin1(kAdvertisement1)) {
        auto err = message.createErrorReply(QDBusError::UnknownInterface,
                                            QStringLiteral("Unknown interface: %1").arg(requestedIface));
        return connection.send(err);
      }
      const QVariantMap m = propertyMap();
      const auto it = m.find(prop);
      if (it == m.cend()) {
        auto err = message.createErrorReply(QDBusError::UnknownProperty,
                                            QStringLiteral("Unknown property: %1").arg(prop));
        return connection.send(err);
      }
      auto reply = message.createReply(QVariant::fromValue(QDBusVariant(it.value())));
      return connection.send(reply);
    }
  }

  return false;
}

} // namespace deskflow::ble::bluez
