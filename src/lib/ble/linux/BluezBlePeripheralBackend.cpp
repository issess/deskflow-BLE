/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/linux/BluezBlePeripheralBackend.h"

#include "base/Log.h"
#include "ble/BleTransport.h"

#include <QBluetoothUuid>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QVariant>
#include <QVariantMap>

namespace deskflow::ble {

namespace {

constexpr const char *kBluezService = "org.bluez";
constexpr const char *kAdvertisingManager = "org.bluez.LEAdvertisingManager1";
constexpr const char *kGattManager = "org.bluez.GattManager1";

QString toUuidString(const QBluetoothUuid &u)
{
  // Qt's toString() yields "{xxxxxxxx-...}" — strip the braces for BlueZ.
  QString s = u.toString();
  if (s.startsWith(QLatin1Char('{')) && s.endsWith(QLatin1Char('}')))
    s = s.mid(1, s.length() - 2);
  return s.toLower();
}

} // namespace

BluezBlePeripheralBackend::BluezBlePeripheralBackend(QObject *parent) : IBlePeripheralBackend(parent)
{
}

BluezBlePeripheralBackend::~BluezBlePeripheralBackend()
{
  stop();
}

void BluezBlePeripheralBackend::buildGattApplication()
{
  using CharFlag = bluez::BluezGattApplication::CharFlag;

  m_app = new bluez::BluezGattApplication(this);
  m_app->setServiceUuid(toUuidString(kServiceUuid));

  m_app->addCharacteristic(toUuidString(kPairingAuthCharUuid),
                           CharFlag::Write | CharFlag::WriteWithoutResponse);
  m_app->addCharacteristic(toUuidString(kPairingStatusCharUuid), CharFlag::Notify);
  m_app->addCharacteristic(toUuidString(kDataDownstreamCharUuid), CharFlag::Notify);
  m_app->addCharacteristic(toUuidString(kDataUpstreamCharUuid),
                           CharFlag::Write | CharFlag::WriteWithoutResponse);
  m_app->addCharacteristic(toUuidString(kControlCharUuid), CharFlag::Notify);

  QObject::connect(m_app, &bluez::BluezGattApplication::characteristicWritten, this,
                   &BluezBlePeripheralBackend::onCharacteristicWritten);
  QObject::connect(m_app, &bluez::BluezGattApplication::subscriberCountChanged, this,
                   &BluezBlePeripheralBackend::onSubscriberCountChanged);
  QObject::connect(m_app, &bluez::BluezGattApplication::mtuChanged, this,
                   &BluezBlePeripheralBackend::onMtuChanged);
}

bool BluezBlePeripheralBackend::start(const QString &localName, const QByteArray &manufacturerPayload)
{
  if (m_started) {
    LOG_WARN("BluezBlePeripheralBackend: already started");
    return false;
  }
  m_localName = localName;
  m_mfgPayload = manufacturerPayload;

  m_adapter = new bluez::BluezAdapter(this);
  QString adapterError;
  if (!m_adapter->start(&adapterError)) {
    LOG_ERR("BluezBlePeripheralBackend: %s", adapterError.toUtf8().constData());
    emitFatal(adapterError);
    return false;
  }
  QObject::connect(m_adapter, &bluez::BluezAdapter::poweredChanged, this,
                   &BluezBlePeripheralBackend::onAdapterPoweredChanged);
  QObject::connect(m_adapter, &bluez::BluezAdapter::bluezDisappeared, this,
                   &BluezBlePeripheralBackend::onBluezDisappeared);

  buildGattApplication();
  if (!m_app->registerOnBus()) {
    emitFatal(QStringLiteral("failed to export GATT application on system bus"));
    return false;
  }

  m_adv = new bluez::BluezAdvertisement(this);
  m_adv->setLocalName(localName);
  m_adv->setServiceUuids({toUuidString(kServiceUuid)});
  m_adv->setManufacturerData(kManufacturerId, manufacturerPayload);
  m_adv->setType(QStringLiteral("peripheral"));
  if (!m_adv->registerOnBus()) {
    emitFatal(QStringLiteral("failed to export Advertisement object"));
    return false;
  }
  QObject::connect(m_adv, &bluez::BluezAdvertisement::released, this,
                   &BluezBlePeripheralBackend::onAdvertisementReleased);

  auto bus = QDBusConnection::systemBus();
  const QString adapterPath = m_adapter->path().path();

  // RegisterAdvertisement (async).
  {
    QDBusInterface mgr(QString::fromLatin1(kBluezService), adapterPath,
                       QString::fromLatin1(kAdvertisingManager), bus);
    if (!mgr.isValid()) {
      emitFatal(QStringLiteral("LEAdvertisingManager1 not available — BlueZ may be too old"));
      return false;
    }
    QVariantMap opts;
    auto pending = mgr.asyncCall(QStringLiteral("RegisterAdvertisement"),
                                 QVariant::fromValue(m_adv->path()), opts);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, this,
                     [this, watcher] { onAdvertisementRegistered(watcher); });
  }

  // RegisterApplication (async).
  {
    QDBusInterface mgr(QString::fromLatin1(kBluezService), adapterPath,
                       QString::fromLatin1(kGattManager), bus);
    if (!mgr.isValid()) {
      emitFatal(QStringLiteral("GattManager1 not available — BlueZ may be too old"));
      return false;
    }
    QVariantMap opts;
    auto pending = mgr.asyncCall(QStringLiteral("RegisterApplication"),
                                 QVariant::fromValue(m_app->rootPath()), opts);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, this,
                     [this, watcher] { onApplicationRegistered(watcher); });
  }

  return true;
}

void BluezBlePeripheralBackend::stop()
{
  if (!m_started && !m_adapter && !m_adv && !m_app)
    return;

  auto bus = QDBusConnection::systemBus();
  if (m_adapter && !m_adapter->path().path().isEmpty()) {
    const QString adapterPath = m_adapter->path().path();
    if (m_advertRegistered) {
      QDBusInterface mgr(QString::fromLatin1(kBluezService), adapterPath,
                         QString::fromLatin1(kAdvertisingManager), bus);
      mgr.asyncCall(QStringLiteral("UnregisterAdvertisement"), QVariant::fromValue(m_adv->path()));
      m_advertRegistered = false;
    }
    if (m_appRegistered) {
      QDBusInterface mgr(QString::fromLatin1(kBluezService), adapterPath,
                         QString::fromLatin1(kGattManager), bus);
      mgr.asyncCall(QStringLiteral("UnregisterApplication"), QVariant::fromValue(m_app->rootPath()));
      m_appRegistered = false;
    }
  }

  if (m_adv) {
    m_adv->unregisterFromBus();
    m_adv->deleteLater();
    m_adv = nullptr;
  }
  if (m_app) {
    m_app->unregisterFromBus();
    m_app->deleteLater();
    m_app = nullptr;
  }
  if (m_adapter) {
    m_adapter->stop();
    m_adapter->deleteLater();
    m_adapter = nullptr;
  }

  m_started = false;
  m_lastSubscriberCount = 0;
  m_mtu = 64;
}

void BluezBlePeripheralBackend::sendPairingStatus(quint8 status)
{
  if (!m_app)
    return;
  m_app->setValue(toUuidString(kPairingStatusCharUuid),
                  QByteArray(1, static_cast<char>(status)), /*notify=*/true);
}

void BluezBlePeripheralBackend::sendDownstream(const QByteArray &chunk)
{
  if (!m_app)
    return;
  m_app->setValue(toUuidString(kDataDownstreamCharUuid), chunk, /*notify=*/true);
}

void BluezBlePeripheralBackend::onAdvertisementRegistered(QDBusPendingCallWatcher *watcher)
{
  watcher->deleteLater();
  QDBusPendingReply<> reply = *watcher;
  if (reply.isError()) {
    const auto err = reply.error();
    LOG_ERR("BluezBlePeripheralBackend: RegisterAdvertisement failed: %s (%s)",
            err.message().toUtf8().constData(), err.name().toUtf8().constData());
    emitFatal(QStringLiteral("RegisterAdvertisement: %1").arg(err.message()));
    return;
  }
  m_advertRegistered = true;
  LOG_NOTE("BluezBlePeripheralBackend: advertisement registered");
  // Started is emitted only once both registrations land. Check.
  if (m_appRegistered && !m_started) {
    m_started = true;
    Q_EMIT started();
  }
}

void BluezBlePeripheralBackend::onApplicationRegistered(QDBusPendingCallWatcher *watcher)
{
  watcher->deleteLater();
  QDBusPendingReply<> reply = *watcher;
  if (reply.isError()) {
    const auto err = reply.error();
    LOG_ERR("BluezBlePeripheralBackend: RegisterApplication failed: %s (%s)",
            err.message().toUtf8().constData(), err.name().toUtf8().constData());
    emitFatal(QStringLiteral("RegisterApplication: %1").arg(err.message()));
    return;
  }
  m_appRegistered = true;
  LOG_NOTE("BluezBlePeripheralBackend: GATT application registered");
  if (m_advertRegistered && !m_started) {
    m_started = true;
    Q_EMIT started();
  }
}

void BluezBlePeripheralBackend::onCharacteristicWritten(const QString &uuid, const QByteArray &value,
                                                        const QString &device)
{
  Q_UNUSED(device);
  if (uuid == toUuidString(kPairingAuthCharUuid)) {
    Q_EMIT pairingAuthWritten(value);
  } else if (uuid == toUuidString(kDataUpstreamCharUuid)) {
    Q_EMIT upstreamWritten(value);
  }
}

void BluezBlePeripheralBackend::onSubscriberCountChanged(int count)
{
  // Emit centralConnected on first-subscribe (0 -> any), centralDisconnected
  // when last unsubscribe pulls the count back to 0.
  if (m_lastSubscriberCount == 0 && count > 0)
    Q_EMIT centralConnected();
  else if (m_lastSubscriberCount > 0 && count == 0)
    Q_EMIT centralDisconnected();
  m_lastSubscriberCount = count;
}

void BluezBlePeripheralBackend::onMtuChanged(int mtu)
{
  if (mtu <= 0 || mtu == m_mtu)
    return;
  m_mtu = mtu;
  Q_EMIT mtuChanged(mtu);
}

void BluezBlePeripheralBackend::onAdvertisementReleased()
{
  // BlueZ released our advert (controller reset / daemon restart).
  m_advertRegistered = false;
  emitFatal(QStringLiteral("BlueZ released the advertisement"));
}

void BluezBlePeripheralBackend::onAdapterPoweredChanged(bool powered)
{
  if (!powered) {
    emitFatal(QStringLiteral("Bluetooth adapter powered off"));
  }
}

void BluezBlePeripheralBackend::onBluezDisappeared()
{
  emitFatal(QStringLiteral("BlueZ daemon disappeared from system bus"));
}

void BluezBlePeripheralBackend::emitFatal(const QString &reason)
{
  // Single-shot: stop() and signal up. Caller (BlePeripheralContext) will
  // tear down and may try the Qt fallback.
  if (!m_started)
    Q_EMIT startFailed(reason);
  // Tear down to release advert/app slots.
  stop();
  if (m_started) {
    // We were running; this is a mid-session failure. Surface as
    // centralDisconnected so the upper layer recovers.
    m_started = false;
  }
}

} // namespace deskflow::ble
