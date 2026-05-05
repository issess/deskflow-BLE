/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/linux/BluezBleCentralBackend.h"

#include "base/Log.h"
#include "ble/BlePairingCode.h"
#include "ble/BleTransport.h"
#include "ble/linux/BluezAdapter.h"
#include "ble/linux/BluezDbusTypes.h"

#include <QBluetoothUuid>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QStringList>

namespace deskflow::ble {

namespace {

constexpr const char *kBluezService = "org.bluez";
constexpr const char *kAdapter1 = "org.bluez.Adapter1";
constexpr const char *kDevice1 = "org.bluez.Device1";
constexpr const char *kGattService1 = "org.bluez.GattService1";
constexpr const char *kGattCharacteristic1 = "org.bluez.GattCharacteristic1";
constexpr const char *kObjectManager = "org.freedesktop.DBus.ObjectManager";
constexpr const char *kProperties = "org.freedesktop.DBus.Properties";

QString toUuidString(const QBluetoothUuid &u)
{
  QString s = u.toString();
  if (s.startsWith(QLatin1Char('{')) && s.endsWith(QLatin1Char('}')))
    s = s.mid(1, s.length() - 2);
  return s.toLower();
}

// Manually walk the a{qv} wire dict that BlueZ uses for ManufacturerData.
// We avoid relying on Qt's auto-marshalling for QMap<quint16, ...> because
// the standard operator>> is reliable only for QMap<QString, QVariant>.
QByteArray manufacturerPayloadForId(const QVariant &v, quint16 wantedId)
{
  if (!v.canConvert<QDBusArgument>())
    return {};
  QDBusArgument arg = v.value<QDBusArgument>();
  arg.beginMap();
  while (!arg.atEnd()) {
    arg.beginMapEntry();
    quint16 key = 0;
    arg >> key;
    QDBusVariant inner;
    arg >> inner;
    arg.endMapEntry();
    if (key == wantedId) {
      const QVariant innerV = inner.variant();
      if (innerV.canConvert<QByteArray>()) {
        arg.endMap();
        return innerV.toByteArray();
      }
      if (innerV.canConvert<QDBusArgument>()) {
        QByteArray bytes;
        innerV.value<QDBusArgument>() >> bytes;
        arg.endMap();
        return bytes;
      }
    }
  }
  arg.endMap();
  return {};
}

} // namespace

BluezBleCentralBackend::BluezBleCentralBackend(QObject *parent) : IBleCentralBackend(parent)
{
  bluez::registerBluezDbusTypes();
}

BluezBleCentralBackend::~BluezBleCentralBackend()
{
  teardownInternal();
}

void BluezBleCentralBackend::setUpstreamLossless(bool lossless)
{
  m_upstreamLossless = lossless;
}

void BluezBleCentralBackend::start(const QString &savedDeviceId, const QString &code,
                                   quint64 directAddress)
{
  if (m_state != State::Idle) {
    LOG_WARN("BluezBleCentralBackend: start called while not idle (state=%d)",
             static_cast<int>(m_state));
    return;
  }
  m_savedDeviceId = savedDeviceId;
  m_pendingCode = code;
  m_expectedHash = code.isEmpty() ? QByteArray() : BlePairingCode::hashPrefix(code);
  m_directAddress = directAddress;

  m_adapter = new bluez::BluezAdapter(this);
  QString err;
  if (!m_adapter->start(&err)) {
    emitFail(err);
    return;
  }

  auto bus = QDBusConnection::systemBus();
  bus.connect(QString::fromLatin1(kBluezService), QStringLiteral("/"),
              QString::fromLatin1(kObjectManager), QStringLiteral("InterfacesAdded"),
              this, SLOT(onInterfacesAdded(QDBusObjectPath, QMap<QString, QVariantMap>)));

  // Direct-address fast path: try the constructed device path before scanning.
  if (m_directAddress != 0) {
    const QString candidate = deviceObjectPathFromAddress(m_adapter->path().path(), m_directAddress);
    LOG_NOTE("BluezBleCentralBackend: trying direct path %s", candidate.toUtf8().constData());
    // GetManagedObjects to confirm the device exists already in BlueZ's cache.
    QDBusInterface om(QString::fromLatin1(kBluezService), QStringLiteral("/"),
                      QString::fromLatin1(kObjectManager), bus);
    QDBusMessage reply = om.call(QStringLiteral("GetManagedObjects"));
    if (reply.type() != QDBusMessage::ErrorMessage && !reply.arguments().isEmpty()) {
      bluez::ManagedObjects objects;
      reply.arguments().first().value<QDBusArgument>() >> objects;
      const auto it = objects.find(QDBusObjectPath(candidate));
      if (it != objects.cend() && it.value().contains(QString::fromLatin1(kDevice1))) {
        connectDevice(candidate);
        return;
      }
    }
    LOG_NOTE("BluezBleCentralBackend: direct path not in cache, falling back to scan");
  }

  startScan();
}

void BluezBleCentralBackend::startScan()
{
  m_state = State::Scanning;
  auto bus = QDBusConnection::systemBus();
  const QString adapterPath = m_adapter->path().path();
  QDBusInterface adapter(QString::fromLatin1(kBluezService), adapterPath,
                         QString::fromLatin1(kAdapter1), bus);

  // Filter by transport=le; UUIDs filter is intentionally not used because
  // BlueZ's UUID match requires the peripheral to put the service UUID in
  // the adv PDU, which is sometimes truncated. ManufacturerData magic is the
  // authoritative match anyway.
  QVariantMap filter;
  filter.insert(QStringLiteral("Transport"), QStringLiteral("le"));
  adapter.call(QStringLiteral("SetDiscoveryFilter"), filter);

  QDBusReply<void> r = adapter.call(QStringLiteral("StartDiscovery"));
  if (!r.isValid()) {
    emitFail(QStringLiteral("StartDiscovery failed: %1").arg(r.error().message()));
    return;
  }

  // Walk existing cache for already-known devices.
  QDBusInterface om(QString::fromLatin1(kBluezService), QStringLiteral("/"),
                    QString::fromLatin1(kObjectManager), bus);
  QDBusMessage reply = om.call(QStringLiteral("GetManagedObjects"));
  if (reply.type() == QDBusMessage::ErrorMessage)
    return;
  bluez::ManagedObjects objects;
  reply.arguments().first().value<QDBusArgument>() >> objects;
  for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
    const auto ifIt = it.value().find(QString::fromLatin1(kDevice1));
    if (ifIt == it.value().cend())
      continue;
    if (considerDevice(it.key().path(), ifIt.value()))
      return;
  }
}

void BluezBleCentralBackend::stopScan()
{
  if (!m_adapter || m_adapter->path().path().isEmpty())
    return;
  auto bus = QDBusConnection::systemBus();
  QDBusInterface adapter(QString::fromLatin1(kBluezService), m_adapter->path().path(),
                         QString::fromLatin1(kAdapter1), bus);
  adapter.call(QStringLiteral("StopDiscovery"));
}

void BluezBleCentralBackend::onInterfacesAdded(const QDBusObjectPath &path,
                                               const QMap<QString, QVariantMap> &interfaces)
{
  const auto it = interfaces.find(QString::fromLatin1(kDevice1));
  if (it == interfaces.cend())
    return;
  if (m_state == State::Scanning)
    considerDevice(path.path(), it.value());
}

bool BluezBleCentralBackend::considerDevice(const QString &path, const QVariantMap &deviceProps)
{
  // Match ManufacturerData{kManufacturerId} prefix (magic + hash prefix).
  const auto mfgIt = deviceProps.find(QStringLiteral("ManufacturerData"));
  if (mfgIt == deviceProps.cend())
    return false;
  const QByteArray payload = manufacturerPayloadForId(mfgIt.value(), kManufacturerId);
  if (payload.size() < 6)
    return false;
  const quint16 magic = (static_cast<quint8>(payload[0]) << 8) | static_cast<quint8>(payload[1]);
  if (magic != kAdvertMagic)
    return false;
  if (!m_expectedHash.isEmpty()) {
    if (payload.mid(2, 4) != m_expectedHash)
      return false;
  }

  LOG_NOTE("BluezBleCentralBackend: found candidate device %s", path.toUtf8().constData());
  stopScan();
  connectDevice(path);
  return true;
}

void BluezBleCentralBackend::connectDevice(const QString &devicePath)
{
  m_devicePath = devicePath;
  m_state = State::Connecting;

  auto bus = QDBusConnection::systemBus();
  bus.connect(QString::fromLatin1(kBluezService), devicePath, QString::fromLatin1(kProperties),
              QStringLiteral("PropertiesChanged"), this,
              SLOT(onDevicePropertiesChanged(QString, QVariantMap, QStringList)));

  QDBusInterface device(QString::fromLatin1(kBluezService), devicePath,
                        QString::fromLatin1(kDevice1), bus);
  // Capture peer address for remembered-peer persistence.
  const QString addrStr = device.property("Address").toString();
  if (!addrStr.isEmpty()) {
    QString hex = addrStr;
    hex.remove(QLatin1Char(':'));
    bool ok = false;
    const quint64 v = hex.toULongLong(&ok, 16);
    if (ok)
      m_peerAddress = v;
  }

  auto pending = device.asyncCall(QStringLiteral("Connect"));
  auto *watcher = new QDBusPendingCallWatcher(pending, this);
  QObject::connect(watcher, &QDBusPendingCallWatcher::finished, this,
                   [this, watcher] { onConnectFinished(watcher); });
}

void BluezBleCentralBackend::onConnectFinished(QDBusPendingCallWatcher *watcher)
{
  watcher->deleteLater();
  QDBusPendingReply<> reply = *watcher;
  if (reply.isError()) {
    emitFail(QStringLiteral("Connect: %1").arg(reply.error().message()));
    return;
  }
  // Check if ServicesResolved is already true; if so, jump straight to discovery.
  auto bus = QDBusConnection::systemBus();
  QDBusInterface device(QString::fromLatin1(kBluezService), m_devicePath,
                        QString::fromLatin1(kDevice1), bus);
  if (device.property("ServicesResolved").toBool()) {
    onServicesResolved();
  } else {
    m_state = State::Resolving;
  }
}

void BluezBleCentralBackend::onDevicePropertiesChanged(const QString &interface,
                                                       const QVariantMap &changed,
                                                       const QStringList &)
{
  if (interface != QString::fromLatin1(kDevice1))
    return;

  const auto resolvedIt = changed.find(QStringLiteral("ServicesResolved"));
  if (resolvedIt != changed.cend() && resolvedIt.value().toBool() && m_state == State::Resolving) {
    onServicesResolved();
  }
  const auto connectedIt = changed.find(QStringLiteral("Connected"));
  if (connectedIt != changed.cend() && !connectedIt.value().toBool()) {
    if (m_state == State::Ready || m_state == State::AwaitingPair) {
      Q_EMIT disconnected();
      teardownInternal();
    } else if (m_state != State::Idle) {
      emitFail(QStringLiteral("device disconnected before pairing completed"));
    }
  }
}

void BluezBleCentralBackend::onServicesResolved()
{
  m_state = State::Discovering;
  discoverCharacteristics();
}

void BluezBleCentralBackend::discoverCharacteristics()
{
  auto bus = QDBusConnection::systemBus();
  QDBusInterface om(QString::fromLatin1(kBluezService), QStringLiteral("/"),
                    QString::fromLatin1(kObjectManager), bus);
  QDBusMessage reply = om.call(QStringLiteral("GetManagedObjects"));
  if (reply.type() == QDBusMessage::ErrorMessage) {
    emitFail(QStringLiteral("GetManagedObjects: %1").arg(reply.errorMessage()));
    return;
  }
  bluez::ManagedObjects objects;
  reply.arguments().first().value<QDBusArgument>() >> objects;

  // Find our service UUID under the device path.
  QString servicePath;
  const QString deviceServicePrefix = m_devicePath + QStringLiteral("/");
  const QString wantedService = toUuidString(kServiceUuid);
  for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
    if (!it.key().path().startsWith(deviceServicePrefix))
      continue;
    const auto svcIt = it.value().find(QString::fromLatin1(kGattService1));
    if (svcIt == it.value().cend())
      continue;
    if (svcIt.value().value(QStringLiteral("UUID")).toString().toLower() == wantedService) {
      servicePath = it.key().path();
      break;
    }
  }
  if (servicePath.isEmpty()) {
    emitFail(QStringLiteral("Deskflow GATT service not found on peer"));
    return;
  }

  // Find each characteristic under the service path.
  const QString servicePrefix = servicePath + QStringLiteral("/");
  for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
    if (!it.key().path().startsWith(servicePrefix))
      continue;
    const auto chIt = it.value().find(QString::fromLatin1(kGattCharacteristic1));
    if (chIt == it.value().cend())
      continue;
    const QString uuid = chIt.value().value(QStringLiteral("UUID")).toString().toLower();
    if (uuid == toUuidString(kPairingAuthCharUuid))
      m_charPairingAuth = it.key().path();
    else if (uuid == toUuidString(kPairingStatusCharUuid))
      m_charPairingStatus = it.key().path();
    else if (uuid == toUuidString(kDataDownstreamCharUuid))
      m_charDataDownstream = it.key().path();
    else if (uuid == toUuidString(kDataUpstreamCharUuid))
      m_charDataUpstream = it.key().path();
    else if (uuid == toUuidString(kControlCharUuid))
      m_charControl = it.key().path();
  }

  if (m_charPairingAuth.isEmpty() || m_charPairingStatus.isEmpty()
      || m_charDataDownstream.isEmpty() || m_charDataUpstream.isEmpty()) {
    emitFail(QStringLiteral("Deskflow characteristics missing on peer"));
    return;
  }

  // Subscribe to PairingStatus and DataDownstream notifies. BlueZ surfaces
  // notifies as PropertiesChanged on Value. Use distinct slots so we know
  // which characteristic the signal originated from.
  auto bus2 = QDBusConnection::systemBus();
  bus2.connect(QString::fromLatin1(kBluezService), m_charPairingStatus,
               QString::fromLatin1(kProperties), QStringLiteral("PropertiesChanged"), this,
               SLOT(onPairingStatusPropertiesChanged(QString, QVariantMap, QStringList)));
  bus2.connect(QString::fromLatin1(kBluezService), m_charDataDownstream,
               QString::fromLatin1(kProperties), QStringLiteral("PropertiesChanged"), this,
               SLOT(onDownstreamPropertiesChanged(QString, QVariantMap, QStringList)));

  QDBusInterface chPS(QString::fromLatin1(kBluezService), m_charPairingStatus,
                      QString::fromLatin1(kGattCharacteristic1), bus2);
  chPS.call(QStringLiteral("StartNotify"));
  QDBusInterface chDD(QString::fromLatin1(kBluezService), m_charDataDownstream,
                      QString::fromLatin1(kGattCharacteristic1), bus2);
  chDD.call(QStringLiteral("StartNotify"));

  // Read MTU if available (BlueZ 5.62+).
  QDBusInterface chDU(QString::fromLatin1(kBluezService), m_charDataUpstream,
                      QString::fromLatin1(kGattCharacteristic1), bus2);
  const QVariant mtuV = chDU.property("MTU");
  if (mtuV.isValid()) {
    const int mtu = mtuV.toInt();
    if (mtu > 0 && mtu != m_mtu) {
      m_mtu = mtu;
      Q_EMIT mtuChanged(mtu);
    }
  }

  if (!m_pendingCode.isEmpty()) {
    m_state = State::AwaitingPair;
    writePairingCode();
  } else {
    // Remembered-peer reconnect: peripheral grants connectivity on subscribe.
    m_state = State::Ready;
    Q_EMIT connected();
  }
}

void BluezBleCentralBackend::writePairingCode()
{
  if (m_charPairingAuth.isEmpty())
    return;
  auto bus = QDBusConnection::systemBus();
  QDBusInterface ch(QString::fromLatin1(kBluezService), m_charPairingAuth,
                    QString::fromLatin1(kGattCharacteristic1), bus);
  QVariantMap opts;
  opts.insert(QStringLiteral("type"), QStringLiteral("request"));
  ch.asyncCall(QStringLiteral("WriteValue"), QVariant::fromValue(m_pendingCode.toUtf8()), opts);
}

void BluezBleCentralBackend::writeUpstream(const QByteArray &chunk)
{
  if (m_charDataUpstream.isEmpty())
    return;
  auto bus = QDBusConnection::systemBus();
  QDBusInterface ch(QString::fromLatin1(kBluezService), m_charDataUpstream,
                    QString::fromLatin1(kGattCharacteristic1), bus);
  QVariantMap opts;
  // BlueZ option semantics: "command" = WriteWithoutResponse, "request" = WriteWithResponse.
  opts.insert(QStringLiteral("type"),
              m_upstreamLossless ? QStringLiteral("request") : QStringLiteral("command"));
  ch.asyncCall(QStringLiteral("WriteValue"), QVariant::fromValue(chunk), opts);
}

static QByteArray valueFromPropertiesChanged(const QVariantMap &changed)
{
  const auto it = changed.find(QStringLiteral("Value"));
  if (it == changed.cend())
    return {};
  if (it.value().canConvert<QByteArray>())
    return it.value().toByteArray();
  if (it.value().canConvert<QDBusArgument>()) {
    QByteArray out;
    it.value().value<QDBusArgument>() >> out;
    return out;
  }
  return {};
}

void BluezBleCentralBackend::onPairingStatusPropertiesChanged(const QString &interface,
                                                              const QVariantMap &changed,
                                                              const QStringList &)
{
  if (interface != QString::fromLatin1(kGattCharacteristic1))
    return;
  const QByteArray v = valueFromPropertiesChanged(changed);
  if (v.isEmpty())
    return;
  const auto status = static_cast<PairingStatus>(static_cast<quint8>(v.at(0)));
  if (status == PairingStatus::Accepted) {
    if (!m_pairingAccepted) {
      m_pairingAccepted = true;
      m_state = State::Ready;
      Q_EMIT connected();
    }
  } else if (status == PairingStatus::Rejected || status == PairingStatus::TimedOut) {
    emitFail(QStringLiteral("pairing rejected by host"));
  }
}

void BluezBleCentralBackend::onDownstreamPropertiesChanged(const QString &interface,
                                                           const QVariantMap &changed,
                                                           const QStringList &)
{
  if (interface != QString::fromLatin1(kGattCharacteristic1))
    return;
  const QByteArray v = valueFromPropertiesChanged(changed);
  if (!v.isEmpty())
    Q_EMIT dataReceived(v);
}

void BluezBleCentralBackend::stop()
{
  if (m_state == State::Ready || m_state == State::AwaitingPair) {
    Q_EMIT disconnected();
  }
  teardownInternal();
}

void BluezBleCentralBackend::teardownInternal()
{
  auto bus = QDBusConnection::systemBus();
  if (!m_charPairingStatus.isEmpty()) {
    QDBusInterface ch(QString::fromLatin1(kBluezService), m_charPairingStatus,
                      QString::fromLatin1(kGattCharacteristic1), bus);
    ch.asyncCall(QStringLiteral("StopNotify"));
    bus.disconnect(QString::fromLatin1(kBluezService), m_charPairingStatus,
                   QString::fromLatin1(kProperties), QStringLiteral("PropertiesChanged"), this,
                   SLOT(onPairingStatusPropertiesChanged(QString, QVariantMap, QStringList)));
  }
  if (!m_charDataDownstream.isEmpty()) {
    QDBusInterface ch(QString::fromLatin1(kBluezService), m_charDataDownstream,
                      QString::fromLatin1(kGattCharacteristic1), bus);
    ch.asyncCall(QStringLiteral("StopNotify"));
    bus.disconnect(QString::fromLatin1(kBluezService), m_charDataDownstream,
                   QString::fromLatin1(kProperties), QStringLiteral("PropertiesChanged"), this,
                   SLOT(onDownstreamPropertiesChanged(QString, QVariantMap, QStringList)));
  }
  if (!m_devicePath.isEmpty()) {
    bus.disconnect(QString::fromLatin1(kBluezService), m_devicePath,
                   QString::fromLatin1(kProperties), QStringLiteral("PropertiesChanged"), this,
                   SLOT(onDevicePropertiesChanged(QString, QVariantMap, QStringList)));
    QDBusInterface device(QString::fromLatin1(kBluezService), m_devicePath,
                          QString::fromLatin1(kDevice1), bus);
    device.asyncCall(QStringLiteral("Disconnect"));
  }
  bus.disconnect(QString::fromLatin1(kBluezService), QStringLiteral("/"),
                 QString::fromLatin1(kObjectManager), QStringLiteral("InterfacesAdded"),
                 this, SLOT(onInterfacesAdded(QDBusObjectPath, QMap<QString, QVariantMap>)));

  stopScan();

  if (m_adapter) {
    m_adapter->stop();
    m_adapter->deleteLater();
    m_adapter = nullptr;
  }

  m_charPairingAuth.clear();
  m_charPairingStatus.clear();
  m_charDataDownstream.clear();
  m_charDataUpstream.clear();
  m_charControl.clear();
  m_devicePath.clear();
  m_state = State::Idle;
  m_pairingAccepted = false;
}

void BluezBleCentralBackend::emitFail(const QString &reason)
{
  LOG_ERR("BluezBleCentralBackend: %s", reason.toUtf8().constData());
  m_state = State::Failed;
  Q_EMIT connectFailed(reason);
  teardownInternal();
}

QString BluezBleCentralBackend::deviceObjectPathFromAddress(const QString &adapterPath, quint64 addr)
{
  // BlueZ's device path convention: <adapter>/dev_AA_BB_CC_DD_EE_FF
  // The hex pairs are upper-case; everything else stays as-is.
  QString hex;
  hex.reserve(17);
  for (int i = 5; i >= 0; --i) {
    if (i != 5)
      hex.append(QLatin1Char('_'));
    const quint8 byte = static_cast<quint8>((addr >> (i * 8)) & 0xFF);
    hex.append(QString::asprintf("%02X", byte));
  }
  return adapterPath + QStringLiteral("/dev_") + hex;
}

} // namespace deskflow::ble
