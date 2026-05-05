/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/linux/BluezAdapter.h"

#include "base/Log.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QStringList>
#include <QVariant>

#include <algorithm>

namespace deskflow::ble::bluez {

namespace {

constexpr const char *kBluezService = "org.bluez";
constexpr const char *kBluezRoot = "/";
constexpr const char *kAdapter1 = "org.bluez.Adapter1";
constexpr const char *kObjectManager = "org.freedesktop.DBus.ObjectManager";
constexpr const char *kProperties = "org.freedesktop.DBus.Properties";

} // namespace

QDBusObjectPath pickAdapter(const ManagedObjects &objects)
{
  // Collect powered Adapter1 paths.
  QList<QString> candidates;
  for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
    const auto ifaceIt = it.value().find(QStringLiteral("org.bluez.Adapter1"));
    if (ifaceIt == it.value().cend())
      continue;
    const auto poweredIt = ifaceIt.value().find(QStringLiteral("Powered"));
    if (poweredIt == ifaceIt.value().cend())
      continue;
    if (!poweredIt.value().toBool())
      continue;
    candidates.push_back(it.key().path());
  }
  if (candidates.isEmpty())
    return QDBusObjectPath();
  std::sort(candidates.begin(), candidates.end());
  return QDBusObjectPath(candidates.first());
}

BluezAdapter::BluezAdapter(QObject *parent) : QObject(parent)
{
  registerBluezDbusTypes();
}

BluezAdapter::~BluezAdapter()
{
  stop();
}

bool BluezAdapter::start(QString *errorOut)
{
  auto fail = [errorOut](const QString &msg) {
    if (errorOut)
      *errorOut = msg;
    return false;
  };

  auto bus = QDBusConnection::systemBus();
  if (!bus.isConnected())
    return fail(QStringLiteral("system DBus is not connected"));

  // Confirm BlueZ is reachable.
  QDBusInterface dbusBus(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                         QStringLiteral("org.freedesktop.DBus"), bus);
  QDBusReply<bool> hasOwner = dbusBus.call(QStringLiteral("NameHasOwner"), QString::fromLatin1(kBluezService));
  if (!hasOwner.isValid() || !hasOwner.value())
    return fail(QStringLiteral("BlueZ daemon (org.bluez) is not running"));

  // GetManagedObjects.
  QDBusInterface om(QString::fromLatin1(kBluezService), QString::fromLatin1(kBluezRoot),
                    QString::fromLatin1(kObjectManager), bus);
  QDBusMessage reply = om.call(QStringLiteral("GetManagedObjects"));
  if (reply.type() == QDBusMessage::ErrorMessage)
    return fail(QStringLiteral("BlueZ GetManagedObjects failed: %1").arg(reply.errorMessage()));
  if (reply.arguments().isEmpty())
    return fail(QStringLiteral("BlueZ GetManagedObjects returned no payload"));

  ManagedObjects objects;
  const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
  arg >> objects;

  m_path = pickAdapter(objects);
  if (m_path.path().isEmpty()) {
    // Distinguish "no adapter at all" from "adapter present but not powered".
    bool sawAny = false;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
      if (it.value().contains(QStringLiteral("org.bluez.Adapter1"))) {
        sawAny = true;
        break;
      }
    }
    return fail(sawAny ? QStringLiteral("Bluetooth is off") : QStringLiteral("no Bluetooth adapter"));
  }

  LOG_NOTE("BluezAdapter: selected %s", m_path.path().toUtf8().constData());

  // Subscribe to PropertiesChanged on the chosen adapter.
  bus.connect(QString::fromLatin1(kBluezService), m_path.path(), QString::fromLatin1(kProperties),
              QStringLiteral("PropertiesChanged"), this,
              SLOT(handlePropertiesChanged(QString, QVariantMap, QStringList)));

  // Subscribe to NameOwnerChanged for org.bluez.
  bus.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
              QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"),
              QStringLiteral("s"), this, SLOT(handleNameOwnerChanged(QString, QString, QString)));

  probeBluezVersion();

  m_running = true;
  return true;
}

void BluezAdapter::stop()
{
  if (!m_running)
    return;
  m_running = false;
  auto bus = QDBusConnection::systemBus();
  bus.disconnect(QString::fromLatin1(kBluezService), m_path.path(), QString::fromLatin1(kProperties),
                 QStringLiteral("PropertiesChanged"), this,
                 SLOT(handlePropertiesChanged(QString, QVariantMap, QStringList)));
  bus.disconnect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                 QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"),
                 QStringLiteral("s"), this, SLOT(handleNameOwnerChanged(QString, QString, QString)));
}

void BluezAdapter::handlePropertiesChanged(const QString &interface, const QVariantMap &changed,
                                           const QStringList &invalidated)
{
  Q_UNUSED(invalidated);
  if (interface != QString::fromLatin1(kAdapter1))
    return;
  const auto it = changed.find(QStringLiteral("Powered"));
  if (it == changed.cend())
    return;
  const bool powered = it.value().toBool();
  LOG_NOTE("BluezAdapter: powered=%d", powered ? 1 : 0);
  Q_EMIT poweredChanged(powered);
}

void BluezAdapter::handleNameOwnerChanged(const QString &name, const QString &oldOwner,
                                          const QString &newOwner)
{
  Q_UNUSED(oldOwner);
  if (name != QString::fromLatin1(kBluezService))
    return;
  if (newOwner.isEmpty()) {
    LOG_WARN("BluezAdapter: org.bluez vanished from system bus");
    Q_EMIT bluezDisappeared();
  }
}

void BluezAdapter::probeBluezVersion()
{
  // BlueZ does not expose its version on DBus in any officially supported way.
  // Pre-5.50 lacks LEAdvertisingManager1; we detect that by a separate probe
  // (presence of the interface on the adapter path) inside the peripheral
  // backend rather than parsing /usr/sbin/bluetoothd --version. For now the
  // major/minor remain 0 and feature gates default to "modern enough."
  m_bluezMajor = 0;
  m_bluezMinor = 0;
}

} // namespace deskflow::ble::bluez
