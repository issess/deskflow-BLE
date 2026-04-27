/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BleSocket.h"

#include "base/Event.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "ble/BlePairingBroker.h"
#include "ble/BlePairingCode.h"
#include "ble/BleProtocolClassifier.h"
#include "ble/BleTransport.h"
#include "ble/IBlePeripheralBackend.h"
#include "common/Settings.h"

#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothAddress>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QCoreApplication>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyConnectionParameters>
#include <QLowEnergyController>
#include <QLowEnergyDescriptor>
#include <QLowEnergyService>
#include <QMetaObject>
#include <QThread>
#include <Qt>
#include <QTextStream>

#include <algorithm>
#include <cstring>

namespace deskflow::ble {

namespace {

bool isNullBleDeviceId(const QString &id)
{
  const QString trimmed = id.trimmed();
  if (trimmed.isEmpty())
    return true;
  if (trimmed.contains(QLatin1Char(':')))
    return false;
  const QBluetoothUuid uuid(trimmed);
  return uuid.isNull();
}

QString deviceIdForPersistence(const QBluetoothDeviceInfo &info)
{
  if (!info.deviceUuid().isNull())
    return info.deviceUuid().toString();
  return info.address().toString();
}

} // namespace

//
// BleSocketContext
//

BleSocketContext::BleSocketContext(BleSocket *owner) : m_owner(owner)
{
}

BleSocketContext::~BleSocketContext() = default;

void BleSocketContext::attachPeripheral(QLowEnergyService *service, int mtu)
{
  m_service = service;
  m_mtu = mtu > 0 ? mtu : 64;
  m_role = Role::Peripheral;
  if (!service)
    return;
  QObject::connect(
      service, &QLowEnergyService::characteristicChanged, this, &BleSocketContext::onCharacteristicChanged
  );
}

void BleSocketContext::attachPeripheralBackend(deskflow::ble::IBlePeripheralBackend *backend, int mtu)
{
  m_peripheralBackend = backend;
  m_mtu = mtu > 0 ? mtu : 64;
  m_role = Role::Peripheral;
}

void BleSocketContext::startCentral(QString savedDeviceId, QString code)
{
  if (isNullBleDeviceId(savedDeviceId))
    savedDeviceId.clear();

  LOG_NOTE("BLE central: startCentral savedDevice=%s codeLen=%d",
           savedDeviceId.toUtf8().constData(), code.size());
  m_role = Role::Central;
  m_pendingCode = code;
  m_expectedHash = BlePairingCode::hashPrefix(code);
  m_savedDeviceId = savedDeviceId;
  m_pairingAccepted = false;

  if (!savedDeviceId.isEmpty() && code.isEmpty()) {
    LOG_NOTE("BLE central: have remembered peer %s — will match it during scan",
             savedDeviceId.toUtf8().constData());
  }

  LOG_NOTE("BLE central: creating device discovery agent");
  m_discovery = new QBluetoothDeviceDiscoveryAgent(this);
  m_discovery->setLowEnergyDiscoveryTimeout(15000);
  QObject::connect(
      m_discovery, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this,
      &BleSocketContext::onScanDeviceDiscovered
  );
  // Windows (and some other stacks) deliver manufacturer data in the scan
  // response after the initial advertisement, triggering deviceUpdated
  // rather than a fresh deviceDiscovered. Treat updates the same way.
  QObject::connect(
      m_discovery, &QBluetoothDeviceDiscoveryAgent::deviceUpdated, this,
      [this](const QBluetoothDeviceInfo &info, QBluetoothDeviceInfo::Fields /*updated*/) {
        onScanDeviceDiscovered(info);
      }
  );
  QObject::connect(m_discovery, &QBluetoothDeviceDiscoveryAgent::finished, this, &BleSocketContext::onScanFinished);
  QObject::connect(
      m_discovery,
      QOverload<QBluetoothDeviceDiscoveryAgent::Error>::of(&QBluetoothDeviceDiscoveryAgent::errorOccurred),
      this,
      [this](QBluetoothDeviceDiscoveryAgent::Error e) {
        LOG_WARN("BLE central: discovery agent error=%d (%s)", static_cast<int>(e),
                 m_discovery ? m_discovery->errorString().toUtf8().constData() : "");
      }
  );
  m_discovery->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
  LOG_NOTE("BLE central: scan started (LE method, 10s timeout)");
}

void BleSocketContext::onScanDeviceDiscovered(const QBluetoothDeviceInfo &info)
{
  if (m_pairingAccepted || m_centralCtl)
    return;
  ++m_scanSeen;
  const bool isLe = (info.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
  const QByteArray md = info.manufacturerData(kManufacturerId);
  const QList<quint16> allMfgIds = info.manufacturerIds();
  QStringList idList;
  for (quint16 id : allMfgIds)
    idList << QString::asprintf("0x%04X", id);
  const QList<QBluetoothUuid> svcUuids = info.serviceUuids();
  const bool hasDeskflowSvc = svcUuids.contains(kServiceUuid);

  LOG_NOTE("BLE central: scan hit name='%s' addr=%s isLE=%d mfgIds=[%s] mfgData(0x%04X)=%d deskflowSvc=%d svcCount=%d",
           info.name().toUtf8().constData(),
           info.deviceUuid().toString().toUtf8().constData(),
           isLe, idList.join(',').toUtf8().constData(), kManufacturerId, md.size(),
           hasDeskflowSvc, svcUuids.size());
  if (!isLe)
    return;
  ++m_scanLeSeen;

  // Remembered-peer fast path: if we have a saved peer ID and the scan
  // surfaces a device whose persisted ID matches, connect immediately.
  // This preserves the real device name/services rather than synthesising
  // a stub QBluetoothDeviceInfo, and ensures the OS BLE stack has a fresh
  // advertising record before we attempt the GATT connection.
  if (!m_savedDeviceId.isEmpty() && m_pendingCode.isEmpty()) {
    const QString persistedNow = deviceIdForPersistence(info);
    const bool matchByPersisted =
        !isNullBleDeviceId(persistedNow) &&
        persistedNow.compare(m_savedDeviceId, Qt::CaseInsensitive) == 0;
    const bool matchByAddress =
        !info.address().isNull() &&
        info.address().toString().compare(m_savedDeviceId, Qt::CaseInsensitive) == 0;
    const bool matchByUuid =
        !info.deviceUuid().isNull() &&
        info.deviceUuid().toString().compare(m_savedDeviceId, Qt::CaseInsensitive) == 0;
    if (matchByPersisted || matchByAddress || matchByUuid) {
      LOG_NOTE("BLE central: remembered peer %s seen as name='%s', connecting",
               m_savedDeviceId.toUtf8().constData(),
               info.name().toUtf8().constData());
      m_discovery->stop();
      connectToCentralDevice(info);
      return;
    }
  }

  const bool magicOk = md.size() >= 6 &&
                       static_cast<quint8>(md[0]) == ((kAdvertMagic >> 8) & 0xFF) &&
                       static_cast<quint8>(md[1]) == (kAdvertMagic & 0xFF);
  const bool hashOk = magicOk && md.mid(2, 4) == m_expectedHash;

  // Primary match path — matching code hash + magic.
  if (hashOk) {
    ++m_scanMagicHit;
    LOG_NOTE("BLE central: host matched by code hash, connecting to %s",
             info.deviceUuid().toString().toUtf8().constData());
    m_discovery->stop();
    connectToCentralDevice(info);
    return;
  }

  // Fallback: if the peer advertises the Deskflow service UUID, attempt a
  // connection anyway. The host will reject a wrong code via PairingStatus
  // and we'll pick the right peer on the next scan iteration. This guards
  // against stacks that drop manufacturer data from the primary advert.
  //
  // Allowed when:
  //   * we have a pending pairing code (initial pair flow), or
  //   * we have a remembered peer ID — the Windows BLE stack often surfaces
  //     scan results with a null/zero address+UUID (re-randomised after a
  //     host restart), so matchByPersisted/Address/Uuid all fail and the
  //     saved-peer fast path above can't fire. In that case the
  //     Deskflow-service-UUID match is the only signal we have for "this is
  //     a Deskflow host"; connect and let the GATT layer take over.
  if (hasDeskflowSvc && (m_pendingCode.size() > 0 || !m_savedDeviceId.isEmpty())) {
    LOG_NOTE("BLE central: Deskflow service UUID matched (mfgData unavailable, %s), trying connection to %s",
             m_pendingCode.size() > 0 ? "pending code" : "remembered-peer",
             info.deviceUuid().toString().toUtf8().constData());
    ++m_scanMagicHit;
    m_discovery->stop();
    connectToCentralDevice(info);
    return;
  }

  if (magicOk) {
    LOG_NOTE("BLE central: skip (Deskflow magic matched but code hash differs)");
  } else if (md.size() > 0 && hasDeskflowSvc) {
    LOG_NOTE("BLE central: skip (Deskflow service UUID present but mfgData magic mismatch and no code/saved peer)");
  } else if (md.size() > 0) {
    LOG_NOTE("BLE central: skip (mfgData present but magic mismatch)");
  } else if (hasDeskflowSvc) {
    LOG_NOTE("BLE central: skip (Deskflow service UUID present but no pairing code and no remembered peer)");
  } else {
    LOG_NOTE("BLE central: skip (no Deskflow mfgData and no Deskflow service UUID)");
  }
}

void BleSocketContext::onScanFinished()
{
  LOG_NOTE("BLE central: scan finished — seen=%d le=%d magicMatches=%d (hasController=%d accepted=%d)",
           m_scanSeen, m_scanLeSeen, m_scanMagicHit, !!m_centralCtl, m_pairingAccepted);
  if (m_centralCtl || m_pairingAccepted)
    return;
  QString reason;
  if (m_scanLeSeen == 0)
    reason = QStringLiteral("no LE devices found — is Bluetooth enabled on this machine?");
  else if (m_scanMagicHit == 0)
    reason = QStringLiteral("no Deskflow host in range — confirm host is advertising (BLE_PAIRING:CODE visible)");
  else
    reason = QStringLiteral("Deskflow host(s) found but none match the entered code");
  failCentral(reason);
}

void BleSocketContext::connectToCentralDevice(const QBluetoothDeviceInfo &info)
{
  LOG_NOTE("BLE central: connectToCentralDevice name='%s' addr=%s",
           info.name().toUtf8().constData(),
           info.deviceUuid().toString().toUtf8().constData());
  m_connectedDeviceId = deviceIdForPersistence(info);
  m_centralCtl = QLowEnergyController::createCentral(info, this);
  if (!m_centralCtl) {
    LOG_ERR("BLE central: createCentral returned null");
    failCentral(QStringLiteral("createCentral null"));
    return;
  }
  QObject::connect(m_centralCtl, &QLowEnergyController::connected, this, &BleSocketContext::onCentralConnected);
  QObject::connect(
      m_centralCtl, &QLowEnergyController::disconnected, this, &BleSocketContext::onCentralDisconnected
  );
  QObject::connect(
      m_centralCtl, &QLowEnergyController::discoveryFinished, this, &BleSocketContext::onServiceDiscoveryFinished
  );
  QObject::connect(
      m_centralCtl,
      QOverload<QLowEnergyController::Error>::of(&QLowEnergyController::errorOccurred), this,
      [this](QLowEnergyController::Error e) {
        LOG_WARN("BLE central error: %d", static_cast<int>(e));
        failCentral(QStringLiteral("controller error"));
      }
  );
  QObject::connect(m_centralCtl, &QLowEnergyController::mtuChanged, this, [this](int mtu) {
    if (mtu > 0) {
      m_mtu = mtu;
      LOG_DEBUG("BLE central MTU=%d", mtu);
    }
  });
  QObject::connect(
      m_centralCtl, &QLowEnergyController::connectionUpdated, this,
      [](const QLowEnergyConnectionParameters &p) {
        LOG_NOTE("BLE central: connection params updated min=%.2fms max=%.2fms latency=%d supervision=%dms",
                 p.minimumInterval(), p.maximumInterval(), p.latency(), p.supervisionTimeout());
      }
  );
  m_centralCtl->connectToDevice();
}

void BleSocketContext::onCentralConnected()
{
  LOG_NOTE("BLE central: TCP-less connection established, discovering services");
  if (m_centralCtl) {
    // Ask the link layer for a short connection interval so notifications
    // flush at HID-input cadence rather than the OS-default 30-50 ms. The
    // peer is free to refuse or pick a value inside the requested range.
    // Spec floor is 7.5 ms; values are in milliseconds with 1.25 ms grain.
    // Latency 0 means the peripheral acks every event (lowest latency,
    // higher power — acceptable for desktop hosts).
    QLowEnergyConnectionParameters p;
    p.setIntervalRange(7.5, 15.0);
    p.setLatency(0);
    p.setSupervisionTimeout(4000);
    LOG_NOTE("BLE central: requesting connection interval 7.5-15ms latency=0 supervision=4000ms");
    m_centralCtl->requestConnectionUpdate(p);
    m_centralCtl->discoverServices();
  }
}

void BleSocketContext::onCentralDisconnected()
{
  if (!m_pairingAccepted) {
    failCentral(QStringLiteral("peer disconnected during pairing"));
  } else if (m_owner) {
    m_owner->notifyDisconnected();
  }
}

void BleSocketContext::onServiceDiscoveryFinished()
{
  LOG_NOTE("BLE central: service discovery finished");
  if (!m_centralCtl)
    return;
  auto *svc = m_centralCtl->createServiceObject(kServiceUuid, this);
  if (!svc) {
    LOG_ERR("BLE central: createServiceObject(Deskflow UUID) returned null — host not advertising the expected service");
    failCentral(QStringLiteral("Deskflow service not advertised on peer"));
    return;
  }
  LOG_NOTE("BLE central: found Deskflow service, discovering details");
  m_service = svc;
  QObject::connect(svc, &QLowEnergyService::stateChanged, this, &BleSocketContext::onServiceStateChanged);
  QObject::connect(svc, &QLowEnergyService::characteristicChanged, this, &BleSocketContext::onCharacteristicChanged);
  QObject::connect(svc, &QLowEnergyService::characteristicWritten, this, &BleSocketContext::onCharacteristicWritten);
  QObject::connect(
      svc,
      QOverload<QLowEnergyService::ServiceError>::of(&QLowEnergyService::errorOccurred), this,
      [](QLowEnergyService::ServiceError e) {
        LOG_WARN("BLE central: service error=%d", static_cast<int>(e));
      }
  );
  svc->discoverDetails();
}

void BleSocketContext::onServiceStateChanged(QLowEnergyService::ServiceState state)
{
  LOG_NOTE("BLE central: service state changed to %d", static_cast<int>(state));
  if (state != QLowEnergyService::ServiceState::RemoteServiceDiscovered)
    return;
  LOG_NOTE("BLE central: service fully discovered, enabling notifications");
  enableNotifications();

  // Remembered-peer reconnect: no pairing code in hand, so the host won't
  // emit PairingStatus::Accepted. Treat the GATT-level connection itself as
  // proof of identity (we already matched the saved peer ID during scan)
  // and surface the connect upwards so the deskflow protocol starts reading
  // the data the host is already streaming on DataDownstream.
  if (m_pendingCode.isEmpty() && !m_savedDeviceId.isEmpty()) {
    LOG_NOTE("BLE central: remembered-peer mode — skipping code handshake, "
             "marking connection accepted");
    m_pairingAccepted = true;
    if (m_owner)
      m_owner->notifyConnected();
    return;
  }

  writePairingCode();
}

void BleSocketContext::enableNotifications()
{
  if (!m_service)
    return;
  const QBluetoothUuid cccdUuid(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
  // CCCD value selects notify (0x0001) vs indicate (0x0002).
  // DataDownstream is the bulk data path; the peripheral sends it as GATT
  // Indicate so each chunk is ACK'd at the link layer. The other two are
  // single-byte status / control channels where Notify is fine.
  struct Target {
    QBluetoothUuid uuid;
    const char *cccd;
  };
  const Target targets[] = {
      {kPairingStatusCharUuid, "0100"},
      {kDataDownstreamCharUuid, "0200"},
      {kControlCharUuid, "0100"},
  };
  for (const auto &t : targets) {
    auto ch = m_service->characteristic(t.uuid);
    if (!ch.isValid())
      continue;
    auto desc = ch.descriptor(cccdUuid);
    if (!desc.isValid())
      continue;
    m_service->writeDescriptor(desc, QByteArray::fromHex(t.cccd));
  }
}

void BleSocketContext::writePairingCode()
{
  if (!m_service || m_pendingCode.isEmpty()) {
    LOG_WARN("BLE central: writePairingCode skipped (service=%p codeEmpty=%d)",
             (void *)m_service.data(), m_pendingCode.isEmpty());
    return;
  }
  auto ch = m_service->characteristic(kPairingAuthCharUuid);
  if (!ch.isValid()) {
    LOG_ERR("BLE central: PairingAuth characteristic not found on service");
    failCentral(QStringLiteral("PairingAuth characteristic missing"));
    return;
  }
  LOG_NOTE("BLE central: writing pairing code to PairingAuth");
  m_service->writeCharacteristic(ch, m_pendingCode.toUtf8(), QLowEnergyService::WriteWithoutResponse);
}

void BleSocketContext::failCentral(const QString &reason)
{
  LOG_WARN("BLE central: failCentral reason='%s'", reason.toUtf8().constData());
  QTextStream(stdout) << "BLE_PAIRING:RESULT=rejected:" << reason << Qt::endl;
  if (!m_owner)
    return;
  BlePairingBroker::instance().clearPendingCode();
  BlePairingBroker::instance().reportResult(false, reason);
  // Post a ConnectionFailed event so Client::connect() reacts.
  auto *info = new IDataSocket::ConnectionFailedInfo(reason.toUtf8().constData());
  m_owner->events()->addEvent(
      Event(EventTypes::DataSocketConnectionFailed, m_owner->getEventTarget(), info, Event::EventFlags::DontFreeData)
  );
  if (m_centralCtl) {
    m_centralCtl->disconnectFromDevice();
    m_centralCtl->deleteLater();
    m_centralCtl.clear();
  }
  if (m_discovery) {
    m_discovery->stop();
    m_discovery->deleteLater();
    m_discovery = nullptr;
  }
}

void BleSocketContext::detach()
{
  if (m_service) {
    QObject::disconnect(m_service, nullptr, this, nullptr);
  }
  m_service.clear();
  m_centralWriteQueue.clear();
  m_centralWriteInFlight = false;
  if (m_centralCtl) {
    QObject::disconnect(m_centralCtl, nullptr, this, nullptr);
    if (m_centralCtl->state() != QLowEnergyController::UnconnectedState)
      m_centralCtl->disconnectFromDevice();
    m_centralCtl->deleteLater();
    m_centralCtl.clear();
  }
  if (m_discovery) {
    m_discovery->stop();
    m_discovery->deleteLater();
    m_discovery = nullptr;
  }
}

void BleSocketContext::enqueueOutbound(QByteArray payload)
{
  const int chunkSize = std::max(1, m_mtu - 3);
  auto chunks = BleFramingWriter::frame(payload, chunkSize);
  LOG_DEBUG("BleSocketContext::enqueueOutbound role=%d payload=%d mtu=%d chunks=%zu",
            static_cast<int>(m_role), payload.size(), m_mtu, chunks.size());

  // Peripheral mode via new backend abstraction.
  if (m_role == Role::Peripheral && m_peripheralBackend) {
    for (const auto &c : chunks)
      m_peripheralBackend->sendDownstream(c);
    return;
  }

  if (!m_service)
    return;
  if (m_role == Role::Central) {
    // Always go through the WriteWithResponse queue. WriteWithoutResponse is
    // fire-and-forget at the BLE link layer and silently drops chunks when the
    // host controller buffer is full or signal degrades briefly. PSF splits a
    // logical message into separate length-prefix and payload write() calls,
    // so a single dropped chunk permanently misaligns the input stream and
    // surfaces later as a bogus 32-bit length (e.g. ASCII "CALV" interpreted
    // as length 0x43414C56). WithResponse uses link-layer ACK + retry.
    for (const auto &c : chunks)
      m_centralWriteQueue.enqueue(c);
    drainCentralWriteQueue();
    return;
  }

  const QBluetoothUuid outUuid =
      (m_role == Role::Peripheral) ? kDataDownstreamCharUuid : kDataUpstreamCharUuid;
  auto ch = m_service->characteristic(outUuid);
  if (!ch.isValid()) {
    LOG_WARN("BLE: outgoing characteristic not valid, dropping %d bytes", payload.size());
    return;
  }
  for (const auto &c : chunks)
    m_service->writeCharacteristic(ch, c);
}

void BleSocketContext::drainCentralWriteQueue()
{
  if (m_role != Role::Central || m_centralWriteInFlight || !m_service || m_centralWriteQueue.isEmpty())
    return;
  auto ch = m_service->characteristic(kDataUpstreamCharUuid);
  if (!ch.isValid()) {
    LOG_WARN("BLE central: DataUpstream characteristic not valid, dropping %d queued chunks",
             m_centralWriteQueue.size());
    m_centralWriteQueue.clear();
    return;
  }
  const QByteArray chunk = m_centralWriteQueue.dequeue();
  m_centralWriteInFlight = true;
  LOG_DEBUG("BLE central: writing upstream chunk len=%d queued=%d", chunk.size(), m_centralWriteQueue.size());
  m_service->writeCharacteristic(ch, chunk, QLowEnergyService::WriteWithResponse);
}

void BleSocketContext::onCharacteristicWritten(const QLowEnergyCharacteristic &ch, const QByteArray &value)
{
  if (m_role != Role::Central || ch.uuid() != kDataUpstreamCharUuid)
    return;
  LOG_DEBUG("BLE central: upstream chunk written len=%d remaining=%d", value.size(), m_centralWriteQueue.size());
  m_centralWriteInFlight = false;
  drainCentralWriteQueue();
}

void BleSocketContext::onCharacteristicChanged(const QLowEnergyCharacteristic &ch, const QByteArray &value)
{
  const auto u = ch.uuid();

  if (m_role == Role::Peripheral) {
    if (u == kDataUpstreamCharUuid && m_owner)
      m_owner->deliverInbound(value);
    return;
  }

  // Central side.
  if (u == kPairingStatusCharUuid) {
    if (value.isEmpty())
      return;
    const auto status = static_cast<PairingStatus>(static_cast<quint8>(value[0]));
    LOG_NOTE("BLE central: PairingStatus notify=%d", static_cast<int>(status));
    if (status == PairingStatus::Accepted) {
      m_pairingAccepted = true;
      BlePairingBroker::instance().clearPendingCode();
      BlePairingBroker::instance().reportResult(true, QString());
      QTextStream(stdout) << "BLE_PAIRING:RESULT=accepted:" << Qt::endl;
      // Now safe to clear the persisted pending code.
      Settings::setValue(Settings::Client::PendingBleCode, QString());
      Settings::save();
      // Remember the peer so next launch skips the code.
      if (m_centralCtl) {
        const auto peerId = !isNullBleDeviceId(m_connectedDeviceId)
                                ? m_connectedDeviceId
                                : m_centralCtl->remoteDeviceUuid().toString();
        if (!isNullBleDeviceId(peerId))
          Settings::setValue(Settings::Client::RemoteBleDevice, peerId);
        Settings::save();
      }
      if (m_owner)
        m_owner->notifyConnected();
    } else if (status == PairingStatus::Rejected) {
      failCentral(QStringLiteral("host rejected code"));
    }
    return;
  }

  if (u == kDataDownstreamCharUuid && m_owner) {
    m_owner->deliverInbound(value);
    return;
  }

  // kControlCharUuid: reserved for future control channel; ignored for now.
}

//
// BleSocket
//

BleSocket::BleSocket(IEventQueue *events) : IDataSocket(events), m_events(events), m_ctx(new BleSocketContext(this))
{
}

BleSocket::~BleSocket()
{
  if (m_ctx) {
    m_ctx->detach();
    m_ctx->deleteLater();
    m_ctx = nullptr;
  }
}

void BleSocket::adoptPeripheralService(QLowEnergyService *service, int mtu)
{
  m_ctx->attachPeripheral(service, mtu);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connected = true;
    m_inputShutdown = false;
    m_outputShutdown = false;
    m_reader.reset();
    m_inputBuffer.clear();
  }
  sendEvent(static_cast<int>(EventTypes::DataSocketConnected));
}

void BleSocket::adoptPeripheralBackend(deskflow::ble::IBlePeripheralBackend *backend, int mtu)
{
  m_ctx->attachPeripheralBackend(backend, mtu);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connected = true;
    m_inputShutdown = false;
    m_outputShutdown = false;
    m_reader.reset();
    m_inputBuffer.clear();
  }
  sendEvent(static_cast<int>(EventTypes::DataSocketConnected));
}

void BleSocket::deliverInbound(const QByteArray &chunk)
{
  bool hasData = false;
  int payloadsParsed = 0;
  int totalPayloadBytes = 0;
  int bufSize = 0;
  bool inputShutdown = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_inputShutdown) {
      LOG_WARN("BleSocket::deliverInbound dropped %d bytes (input shutdown)", chunk.size());
      return;
    }
    m_reader.feed(chunk);
    QByteArray payload;
    while (m_reader.next(payload)) {
      m_inputBuffer.append(payload);
      totalPayloadBytes += payload.size();
      ++payloadsParsed;
      hasData = true;
    }
    bufSize = m_inputBuffer.size();
    inputShutdown = m_inputShutdown;
  }
  LOG_DEBUG("BleSocket::deliverInbound chunk=%d parsed=%d payloadBytes=%d bufNow=%d shutdown=%d hasData=%d",
            chunk.size(), payloadsParsed, totalPayloadBytes, bufSize, inputShutdown, hasData);
  if (hasData)
    sendEvent(static_cast<int>(EventTypes::StreamInputReady));
}

void BleSocket::updateMtu(int mtu)
{
  if (m_ctx && mtu > 0)
    m_ctx->setMtu(mtu);
}

void BleSocket::notifyDisconnected()
{
  bool wasConnected;
  int bufLeft = 0;
  bool inShut = false;
  bool outShut = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    wasConnected = m_connected;
    m_connected = false;
    bufLeft = m_inputBuffer.size();
    inShut = m_inputShutdown;
    outShut = m_outputShutdown;
  }
  LOG_NOTE("BleSocket::notifyDisconnected wasConnected=%d unread=%d inShut=%d outShut=%d",
           wasConnected, bufLeft, inShut, outShut);
  if (wasConnected)
    sendEvent(static_cast<int>(EventTypes::SocketDisconnected));
}

void BleSocket::notifyConnected()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connected = true;
    m_inputShutdown = false;
    m_outputShutdown = false;
    m_reader.reset();
    m_inputBuffer.clear();
  }
  sendEvent(static_cast<int>(EventTypes::DataSocketConnected));
}

void BleSocket::bind(const NetworkAddress &)
{
  // peripheral-side socket is created already bound
}

void BleSocket::close()
{
  bool wasConnected;
  int bufLeft = 0;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    wasConnected = m_connected;
    bufLeft = m_inputBuffer.size();
    m_connected = false;
    m_inputShutdown = true;
    m_outputShutdown = true;
    m_inputBuffer.clear();
    m_reader.reset();
  }
  LOG_NOTE("BleSocket::close wasConnected=%d unread=%d", wasConnected, bufLeft);
  if (m_ctx)
    m_ctx->detach();
  if (wasConnected)
    sendEvent(static_cast<int>(EventTypes::SocketDisconnected));
}

void *BleSocket::getEventTarget() const
{
  return const_cast<BleSocket *>(this);
}

void BleSocket::connect(const NetworkAddress &)
{
  LOG_NOTE("BleSocket::connect called; settings file=%s",
           Settings::settingsFile().toUtf8().constData());
  QString savedDevice = Settings::value(Settings::Client::RemoteBleDevice).toString();
  // Pending code is written by the GUI dialog to Settings so it crosses the
  // process boundary. Broker is a fallback if both run in the same process.
  QString code = Settings::value(Settings::Client::PendingBleCode).toString();
  const QString brokerCode = BlePairingBroker::instance().pendingCode();
  LOG_NOTE("BleSocket::connect source settings-codeLen=%d broker-codeLen=%d",
           code.size(), brokerCode.size());
  if (code.isEmpty())
    code = brokerCode;
  if (!code.isEmpty() || isNullBleDeviceId(savedDevice)) {
    if (!savedDevice.isEmpty())
      Settings::setValue(Settings::Client::RemoteBleDevice);
    savedDevice.clear();
  }

  LOG_NOTE("BleSocket::connect savedDevice=%s codeLen=%d",
           savedDevice.toUtf8().constData(), code.size());

  if (savedDevice.isEmpty() && code.isEmpty()) {
    LOG_WARN("BLE: no pairing code and no remembered peer — cannot connect");
    auto *info = new ConnectionFailedInfo("BLE: no pairing code entered and no remembered peer");
    m_events->addEvent(
        Event(EventTypes::DataSocketConnectionFailed, getEventTarget(), info, Event::EventFlags::DontFreeData)
    );
    return;
  }

  // Keep the pending code in Settings until pairing is actually accepted.
  // Client retries connect on every failure/timeout, and clearing too early
  // leaves the retry without a code. We clear it when PairingStatus notifies
  // Accepted (see onCharacteristicChanged).

  LOG_NOTE("BleSocket::connect dispatching startCentral on Qt thread");
  QMetaObject::invokeMethod(
      m_ctx, "startCentral", Qt::QueuedConnection, Q_ARG(QString, savedDevice), Q_ARG(QString, code)
  );
}

bool BleSocket::isFatal() const
{
  return false;
}

uint32_t BleSocket::read(void *buffer, uint32_t n)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_inputBuffer.isEmpty())
    return 0;
  const uint32_t take = std::min<uint32_t>(n, static_cast<uint32_t>(m_inputBuffer.size()));
  if (buffer)
    std::memcpy(buffer, m_inputBuffer.constData(), take);
  m_inputBuffer.remove(0, static_cast<int>(take));
  return take;
}

void BleSocket::write(const void *buffer, uint32_t n)
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_outputShutdown || !m_connected) {
      LOG_WARN("BLE socket write rejected (connected=%d inputShutdown=%d outputShutdown=%d bytes=%u)",
               m_connected, m_inputShutdown, m_outputShutdown, n);
      sendEvent(static_cast<int>(EventTypes::StreamOutputError));
      return;
    }
  }
  LOG_DEBUG("BleSocket::write n=%u", n);
  QByteArray payload(reinterpret_cast<const char *>(buffer), static_cast<int>(n));
  if (isNoopPsfFrame(payload)) {
    LOG_DEBUG("BleSocket::write suppressed BLE no-op frame");
    sendEvent(static_cast<int>(EventTypes::StreamOutputFlushed));
    return;
  }
  // Hop to the Qt thread that owns the controller before touching the service.
  QMetaObject::invokeMethod(
      m_ctx, "enqueueOutbound", Qt::QueuedConnection, Q_ARG(QByteArray, payload)
  );
  sendEvent(static_cast<int>(EventTypes::StreamOutputFlushed));
}

void BleSocket::flush()
{
  // Qt event loop drains outbound queue asynchronously; nothing to do here.
}

void BleSocket::shutdownInput()
{
  int bufLeft = 0;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    bufLeft = m_inputBuffer.size();
    m_inputShutdown = true;
    m_inputBuffer.clear();
    m_reader.reset();
  }
  LOG_NOTE("BleSocket::shutdownInput discarded=%d", bufLeft);
  sendEvent(static_cast<int>(EventTypes::StreamInputShutdown));
}

void BleSocket::shutdownOutput()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outputShutdown = true;
  }
  LOG_NOTE("BleSocket::shutdownOutput");
  sendEvent(static_cast<int>(EventTypes::StreamOutputShutdown));
}

bool BleSocket::isReady() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return !m_inputBuffer.isEmpty();
}

uint32_t BleSocket::getSize() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<uint32_t>(m_inputBuffer.size());
}

void BleSocket::sendEvent(int type)
{
  m_events->addEvent(Event(static_cast<EventTypes>(type), getEventTarget()));
}

} // namespace deskflow::ble
