/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/qt/QtBlePeripheralBackend.h"

#include "base/Log.h"
#include "ble/BleTransport.h"

#include <QBluetoothLocalDevice>
#include <QBluetoothUuid>
#include <QLowEnergyAdvertisingData>
#include <QLowEnergyAdvertisingParameters>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyCharacteristicData>
#include <QLowEnergyController>
#include <QLowEnergyDescriptorData>
#include <QLowEnergyService>
#include <QLowEnergyServiceData>
#include <QTimer>

namespace deskflow::ble {

QtBlePeripheralBackend::QtBlePeripheralBackend(QObject *parent) : IBlePeripheralBackend(parent)
{
}

QtBlePeripheralBackend::~QtBlePeripheralBackend()
{
  stop();
}

bool QtBlePeripheralBackend::start(const QString &localName, const QByteArray &manufacturerPayload)
{
  LOG_NOTE("QtBlePeripheralBackend::start");
  if (m_started) {
    LOG_WARN("QtBlePeripheralBackend: already started");
    return false;
  }
  m_localName = localName;
  m_mfgPayload = manufacturerPayload;

  const auto adapters = QBluetoothLocalDevice::allDevices();
  if (adapters.isEmpty()) {
    Q_EMIT startFailed(QStringLiteral("no Bluetooth adapter"));
    return false;
  }
  for (const auto &ad : adapters) {
    LOG_NOTE("BLE adapter: '%s' addr=%s",
             ad.name().toUtf8().constData(), ad.address().toString().toUtf8().constData());
  }
  QBluetoothLocalDevice local;
  if (local.hostMode() == QBluetoothLocalDevice::HostPoweredOff) {
    Q_EMIT startFailed(QStringLiteral("Bluetooth is off"));
    return false;
  }

  // Characteristics with explicit initial values (Qt WinRT backend needs
  // these non-empty).
  const QByteArray oneZero(1, '\0');
  const QByteArray sixZero(6, '\0');

  QLowEnergyCharacteristicData pairingAuth;
  pairingAuth.setUuid(kPairingAuthCharUuid);
  pairingAuth.setProperties(QLowEnergyCharacteristic::Write | QLowEnergyCharacteristic::WriteNoResponse);
  pairingAuth.setValue(sixZero);
  pairingAuth.setValueLength(1, kPairingCodeDigits * 4);

  QLowEnergyCharacteristicData pairingStatus;
  pairingStatus.setUuid(kPairingStatusCharUuid);
  pairingStatus.setProperties(QLowEnergyCharacteristic::Notify);
  pairingStatus.setValue(oneZero);
  pairingStatus.setValueLength(1, 1);

  QLowEnergyCharacteristicData dataDownstream;
  dataDownstream.setUuid(kDataDownstreamCharUuid);
  // Notify (fire-and-forget). Loss is detected and resynced at the
  // application layer via BleFraming packet IDs (BleFramingReader::feedChunk
  // drops stale chunks and discards in-progress assembly on an id-gap), so
  // a dropped chunk costs one logical frame instead of desyncing PSF.
  dataDownstream.setProperties(QLowEnergyCharacteristic::Notify);
  dataDownstream.setValue(oneZero);
  dataDownstream.setValueLength(1, 244);

  QLowEnergyCharacteristicData dataUpstream;
  dataUpstream.setUuid(kDataUpstreamCharUuid);
  dataUpstream.setProperties(QLowEnergyCharacteristic::Write | QLowEnergyCharacteristic::WriteNoResponse);
  dataUpstream.setValue(oneZero);
  dataUpstream.setValueLength(1, 244);

  QLowEnergyCharacteristicData control;
  control.setUuid(kControlCharUuid);
  control.setProperties(QLowEnergyCharacteristic::Notify);
  control.setValue(oneZero);
  control.setValueLength(1, 1);

  QLowEnergyServiceData svcData;
  svcData.setType(QLowEnergyServiceData::ServiceTypePrimary);
  svcData.setUuid(kServiceUuid);
  svcData.addCharacteristic(pairingAuth);
  svcData.addCharacteristic(pairingStatus);
  svcData.addCharacteristic(dataDownstream);
  svcData.addCharacteristic(dataUpstream);
  svcData.addCharacteristic(control);

  m_controller = QLowEnergyController::createPeripheral(this);
  if (!m_controller) {
    Q_EMIT startFailed(QStringLiteral("createPeripheral returned null"));
    return false;
  }
  QObject::connect(m_controller, &QLowEnergyController::disconnected, this,
                   [this] { Q_EMIT centralDisconnected(); });
  QObject::connect(m_controller, &QLowEnergyController::mtuChanged, this, [this](int mtu) {
    m_mtu = mtu > 0 ? mtu : 64;
    Q_EMIT mtuChanged(m_mtu);
  });
  QObject::connect(m_controller,
                   QOverload<QLowEnergyController::Error>::of(&QLowEnergyController::errorOccurred), this,
                   [this](QLowEnergyController::Error e) {
                     LOG_WARN("QtBlePeripheralBackend: controller error=%d", static_cast<int>(e));
                     if (e == QLowEnergyController::Error::AdvertisingError)
                       scheduleRetry();
                   });

  m_service = m_controller->addService(svcData, this);
  if (!m_service) {
    Q_EMIT startFailed(QStringLiteral("addService failed"));
    return false;
  }
  QObject::connect(m_service, &QLowEnergyService::characteristicChanged, this,
                   &QtBlePeripheralBackend::onServiceCharacteristicChanged);

  // Defer startAdvertising to give the stack time to register the service.
  QTimer::singleShot(1500, this, [this] {
    if (!m_controller)
      return;
    QLowEnergyAdvertisingData advData;
    advData.setDiscoverability(QLowEnergyAdvertisingData::DiscoverabilityGeneral);
    advData.setServices({kServiceUuid});

    QLowEnergyAdvertisingData scanResp;
    scanResp.setLocalName(m_localName);
    scanResp.setManufacturerData(kManufacturerId, m_mfgPayload);

    QLowEnergyAdvertisingParameters params;
    params.setMode(QLowEnergyAdvertisingParameters::AdvInd);
    params.setInterval(160, 320);

    m_controller->startAdvertising(params, advData, scanResp);
    LOG_NOTE("QtBlePeripheralBackend: startAdvertising returned, state=%d",
             static_cast<int>(m_controller->state()));
    if (m_controller->state() == QLowEnergyController::AdvertisingState) {
      m_started = true;
      Q_EMIT started();
    }
  });
  return true;
}

void QtBlePeripheralBackend::stop()
{
  if (m_advRetryTimer)
    m_advRetryTimer->stop();
  if (m_controller) {
    if (m_controller->state() != QLowEnergyController::UnconnectedState) {
      m_controller->stopAdvertising();
      m_controller->disconnectFromDevice();
    }
    m_controller->deleteLater();
    m_controller.clear();
  }
  if (m_service) {
    m_service->deleteLater();
    m_service.clear();
  }
  m_started = false;
}

void QtBlePeripheralBackend::sendPairingStatus(quint8 status)
{
  if (!m_service)
    return;
  auto ch = m_service->characteristic(kPairingStatusCharUuid);
  if (!ch.isValid())
    return;
  m_service->writeCharacteristic(ch, QByteArray(1, static_cast<char>(status)));
}

void QtBlePeripheralBackend::sendDownstream(const QByteArray &chunk)
{
  if (!m_service)
    return;
  auto ch = m_service->characteristic(kDataDownstreamCharUuid);
  if (!ch.isValid())
    return;
  m_service->writeCharacteristic(ch, chunk);
}

void QtBlePeripheralBackend::onServiceCharacteristicChanged(
    const QLowEnergyCharacteristic &ch, const QByteArray &value)
{
  const auto u = ch.uuid();
  if (u == kPairingAuthCharUuid)
    Q_EMIT pairingAuthWritten(value);
  else if (u == kDataUpstreamCharUuid)
    Q_EMIT upstreamWritten(value);
}

void QtBlePeripheralBackend::scheduleRetry()
{
  if (m_advRetry >= 3) {
    Q_EMIT startFailed(QStringLiteral("advertising failed after retries"));
    return;
  }
  ++m_advRetry;
  if (!m_advRetryTimer) {
    m_advRetryTimer = new QTimer(this);
    m_advRetryTimer->setSingleShot(true);
    QObject::connect(m_advRetryTimer, &QTimer::timeout, this, &QtBlePeripheralBackend::retryAdvertising);
  }
  m_advRetryTimer->start(700);
}

void QtBlePeripheralBackend::retryAdvertising()
{
  if (!m_controller)
    return;
  QLowEnergyAdvertisingData advData;
  advData.setDiscoverability(QLowEnergyAdvertisingData::DiscoverabilityGeneral);
  advData.setLocalName(m_localName);
  QLowEnergyAdvertisingData scanResp;
  scanResp.setManufacturerData(kManufacturerId, m_mfgPayload);
  scanResp.setServices({kServiceUuid});
  QLowEnergyAdvertisingParameters params;
  params.setMode(QLowEnergyAdvertisingParameters::AdvInd);
  params.setInterval(160, 320);
  m_controller->startAdvertising(params, advData, scanResp);
}

} // namespace deskflow::ble
