/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/linux/BluezGattApplication.h"

#include "base/Log.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>

namespace deskflow::ble::bluez {

namespace {

constexpr const char *kObjectManager = "org.freedesktop.DBus.ObjectManager";
constexpr const char *kProperties = "org.freedesktop.DBus.Properties";
constexpr const char *kIntrospectable = "org.freedesktop.DBus.Introspectable";
constexpr const char *kGattService1 = "org.bluez.GattService1";
constexpr const char *kGattCharacteristic1 = "org.bluez.GattCharacteristic1";

const QString kRootIntrospect = QStringLiteral(
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\""
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.DBus.ObjectManager\">\n"
    "    <method name=\"GetManagedObjects\">\n"
    "      <arg name=\"objects\" type=\"a{oa{sa{sv}}}\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "</node>\n");

const QString kServiceIntrospect = QStringLiteral(
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\""
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.bluez.GattService1\">\n"
    "    <property name=\"UUID\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Primary\" type=\"b\" access=\"read\"/>\n"
    "  </interface>\n"
    "</node>\n");

const QString kCharIntrospect = QStringLiteral(
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\""
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.bluez.GattCharacteristic1\">\n"
    "    <method name=\"ReadValue\">\n"
    "      <arg name=\"options\" type=\"a{sv}\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"ay\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"WriteValue\">\n"
    "      <arg name=\"value\" type=\"ay\" direction=\"in\"/>\n"
    "      <arg name=\"options\" type=\"a{sv}\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"StartNotify\"/>\n"
    "    <method name=\"StopNotify\"/>\n"
    "    <property name=\"UUID\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Service\" type=\"o\" access=\"read\"/>\n"
    "    <property name=\"Flags\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"Notifying\" type=\"b\" access=\"read\"/>\n"
    "  </interface>\n"
    "</node>\n");

} // namespace

BluezGattApplication::BluezGattApplication(QObject *parent) : QDBusVirtualObject(parent)
{
  registerBluezDbusTypes();
}

BluezGattApplication::~BluezGattApplication()
{
  unregisterFromBus();
}

void BluezGattApplication::addCharacteristic(const QString &uuid, CharFlags flags)
{
  if (m_registered)
    return;
  CharState s;
  s.uuid = uuid.toLower();
  s.flags = flags;
  s.path = QStringLiteral("%1/service0/char%2").arg(m_root).arg(m_chars.size());
  m_chars.append(std::move(s));
}

bool BluezGattApplication::registerOnBus()
{
  if (m_registered)
    return true;
  auto bus = QDBusConnection::systemBus();
  // SubPath flag means we handle every path at-or-below m_root.
  if (!bus.registerVirtualObject(m_root, this, QDBusConnection::SubPath)) {
    LOG_ERR("BluezGattApplication: registerVirtualObject(%s) failed: %s",
            m_root.toUtf8().constData(), bus.lastError().message().toUtf8().constData());
    return false;
  }
  m_registered = true;
  return true;
}

void BluezGattApplication::unregisterFromBus()
{
  if (!m_registered)
    return;
  m_registered = false;
  auto bus = QDBusConnection::systemBus();
  bus.unregisterObject(m_root);
}

bool BluezGattApplication::setValue(const QString &uuid, const QByteArray &value, bool notify)
{
  const QString needle = uuid.toLower();
  for (int i = 0; i < m_chars.size(); ++i) {
    if (m_chars[i].uuid != needle)
      continue;
    m_chars[i].value = value;
    if (notify && m_chars[i].notifying)
      emitValueChanged(i);
    return true;
  }
  return false;
}

int BluezGattApplication::subscriberCount() const
{
  int n = 0;
  for (const auto &c : m_chars)
    if (c.notifying)
      ++n;
  return n;
}

QString BluezGattApplication::introspect(const QString &path) const
{
  const auto lookup = classifyPath(path);
  switch (lookup.role) {
  case PathRole::Root:
    return kRootIntrospect;
  case PathRole::Service:
    return kServiceIntrospect;
  case PathRole::Characteristic:
    return kCharIntrospect;
  case PathRole::Unknown:
  default:
    // Empty introspection — no node here. SubPath registration may receive
    // probes for unknown sub-paths; respond with a minimal empty node.
    return QStringLiteral("<node/>");
  }
}

bool BluezGattApplication::handleMessage(const QDBusMessage &message, const QDBusConnection &connection)
{
  const auto lookup = classifyPath(message.path());
  const QString iface = message.interface();
  const QString member = message.member();

  if (iface == QString::fromLatin1(kIntrospectable) && member == QStringLiteral("Introspect")) {
    auto reply = message.createReply(QVariant(introspect(message.path())));
    return connection.send(reply);
  }

  if (lookup.role == PathRole::Root) {
    if (iface == QString::fromLatin1(kObjectManager) && member == QStringLiteral("GetManagedObjects"))
      return handleObjectManagerGetManagedObjects(message, connection);
    return false;
  }

  if (iface == QString::fromLatin1(kProperties))
    return handleProperties(message, connection, lookup);

  if (lookup.role == PathRole::Service)
    return handleService(message, connection);

  if (lookup.role == PathRole::Characteristic)
    return handleCharacteristic(message, connection, lookup.charIndex);

  return false;
}

BluezGattApplication::PathLookup BluezGattApplication::classifyPath(const QString &path) const
{
  PathLookup r;
  if (path == m_root) {
    r.role = PathRole::Root;
    return r;
  }
  const QString servicePath = m_root + QStringLiteral("/service0");
  if (path == servicePath) {
    r.role = PathRole::Service;
    return r;
  }
  for (int i = 0; i < m_chars.size(); ++i) {
    if (m_chars[i].path == path) {
      r.role = PathRole::Characteristic;
      r.charIndex = i;
      return r;
    }
  }
  return r;
}

bool BluezGattApplication::handleObjectManagerGetManagedObjects(const QDBusMessage &m,
                                                                const QDBusConnection &c)
{
  ManagedObjects out;

  // service0
  {
    InterfaceMap ifaces;
    ifaces.insert(QString::fromLatin1(kGattService1), servicePropertyMap());
    out.insert(QDBusObjectPath(m_root + QStringLiteral("/service0")), ifaces);
  }

  // each characteristic
  for (int i = 0; i < m_chars.size(); ++i) {
    InterfaceMap ifaces;
    ifaces.insert(QString::fromLatin1(kGattCharacteristic1), characteristicPropertyMap(i));
    out.insert(QDBusObjectPath(m_chars[i].path), ifaces);
  }

  auto reply = m.createReply(QVariant::fromValue(out));
  return c.send(reply);
}

QVariantMap BluezGattApplication::servicePropertyMap() const
{
  QVariantMap props;
  props.insert(QStringLiteral("UUID"), m_serviceUuid);
  props.insert(QStringLiteral("Primary"), true);
  return props;
}

QStringList BluezGattApplication::flagStrings(CharFlags f) const
{
  QStringList out;
  if (f.testFlag(CharFlag::Read))
    out << QStringLiteral("read");
  if (f.testFlag(CharFlag::Write))
    out << QStringLiteral("write");
  if (f.testFlag(CharFlag::WriteWithoutResponse))
    out << QStringLiteral("write-without-response");
  if (f.testFlag(CharFlag::Notify))
    out << QStringLiteral("notify");
  if (f.testFlag(CharFlag::Indicate))
    out << QStringLiteral("indicate");
  return out;
}

QVariantMap BluezGattApplication::characteristicPropertyMap(int charIndex) const
{
  const auto &c = m_chars[charIndex];
  QVariantMap props;
  props.insert(QStringLiteral("UUID"), c.uuid);
  props.insert(QStringLiteral("Service"), QVariant::fromValue(QDBusObjectPath(m_root + QStringLiteral("/service0"))));
  props.insert(QStringLiteral("Flags"), flagStrings(c.flags));
  props.insert(QStringLiteral("Notifying"), c.notifying);
  // Value is intentionally omitted from GetManagedObjects payload — BlueZ
  // reads it on demand and we want to control when it's exposed.
  return props;
}

bool BluezGattApplication::handleProperties(const QDBusMessage &m, const QDBusConnection &c,
                                            const PathLookup &lookup)
{
  const QString member = m.member();
  if (member != QStringLiteral("Get") && member != QStringLiteral("GetAll"))
    return false;

  const QString requestedIface = m.arguments().value(0).toString();

  QVariantMap props;
  if (lookup.role == PathRole::Service && requestedIface == QString::fromLatin1(kGattService1)) {
    props = servicePropertyMap();
  } else if (lookup.role == PathRole::Characteristic
             && requestedIface == QString::fromLatin1(kGattCharacteristic1)) {
    props = characteristicPropertyMap(lookup.charIndex);
    // Include Value in property responses (but not in GetManagedObjects).
    props.insert(QStringLiteral("Value"), m_chars[lookup.charIndex].value);
  } else {
    auto err = m.createErrorReply(QDBusError::UnknownInterface,
                                  QStringLiteral("Unknown interface: %1").arg(requestedIface));
    return c.send(err);
  }

  if (member == QStringLiteral("GetAll")) {
    auto reply = m.createReply(QVariant::fromValue(props));
    return c.send(reply);
  }
  // Get
  const QString prop = m.arguments().value(1).toString();
  const auto it = props.find(prop);
  if (it == props.cend()) {
    auto err = m.createErrorReply(QDBusError::UnknownProperty,
                                  QStringLiteral("Unknown property: %1").arg(prop));
    return c.send(err);
  }
  auto reply = m.createReply(QVariant::fromValue(QDBusVariant(it.value())));
  return c.send(reply);
}

bool BluezGattApplication::handleService(const QDBusMessage &m, const QDBusConnection &c)
{
  Q_UNUSED(c);
  Q_UNUSED(m);
  // GattService1 has no methods. Properties handled in handleProperties above.
  return false;
}

bool BluezGattApplication::handleCharacteristic(const QDBusMessage &m, const QDBusConnection &c,
                                                int charIndex)
{
  if (m.interface() != QString::fromLatin1(kGattCharacteristic1))
    return false;

  const QString member = m.member();
  auto &state = m_chars[charIndex];

  if (member == QStringLiteral("ReadValue")) {
    auto reply = m.createReply(QVariant::fromValue(state.value));
    return c.send(reply);
  }

  if (member == QStringLiteral("WriteValue")) {
    const QByteArray value = m.arguments().value(0).toByteArray();
    const QVariantMap options = qdbus_cast<QVariantMap>(m.arguments().value(1).value<QDBusArgument>());
    state.value = value;
    // Pull MTU + device from options when present.
    const auto mtuIt = options.find(QStringLiteral("mtu"));
    if (mtuIt != options.cend()) {
      const int mtu = mtuIt.value().toInt();
      if (mtu > 0 && mtu != m_observedMtu) {
        m_observedMtu = mtu;
        Q_EMIT mtuChanged(mtu);
      }
    }
    QString device;
    const auto devIt = options.find(QStringLiteral("device"));
    if (devIt != options.cend())
      device = devIt.value().value<QDBusObjectPath>().path();

    Q_EMIT characteristicWritten(state.uuid, value, device);

    auto reply = m.createReply();
    return c.send(reply);
  }

  if (member == QStringLiteral("StartNotify")) {
    if (!state.notifying) {
      state.notifying = true;
      const int total = subscriberCount();
      LOG_NOTE("BluezGattApplication: StartNotify on %s (notifying total=%d)",
               state.uuid.toUtf8().constData(), total);
      Q_EMIT subscriberCountChanged(total);
    }
    auto reply = m.createReply();
    return c.send(reply);
  }

  if (member == QStringLiteral("StopNotify")) {
    if (state.notifying) {
      state.notifying = false;
      const int total = subscriberCount();
      LOG_NOTE("BluezGattApplication: StopNotify on %s (notifying total=%d)",
               state.uuid.toUtf8().constData(), total);
      Q_EMIT subscriberCountChanged(total);
    }
    auto reply = m.createReply();
    return c.send(reply);
  }

  return false;
}

void BluezGattApplication::emitValueChanged(int charIndex)
{
  const auto &c = m_chars[charIndex];
  // PropertiesChanged signal: org.freedesktop.DBus.Properties.PropertiesChanged
  // Args: interface_name (s), changed_properties (a{sv}), invalidated (as).
  auto signal = QDBusMessage::createSignal(c.path, QString::fromLatin1(kProperties),
                                           QStringLiteral("PropertiesChanged"));
  QVariantMap changed;
  changed.insert(QStringLiteral("Value"), c.value);
  signal << QString::fromLatin1(kGattCharacteristic1)
         << QVariant::fromValue(changed)
         << QStringList();
  QDBusConnection::systemBus().send(signal);
}

} // namespace deskflow::ble::bluez
