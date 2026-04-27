/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ble/BleFraming.h"
#include "net/IDataSocket.h"

#include <QLowEnergyService>

#include <QByteArray>
#include <QBluetoothDeviceInfo>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <mutex>

class IEventQueue;
class QBluetoothDeviceDiscoveryAgent;
class QLowEnergyService;
class QLowEnergyController;
class QLowEnergyCharacteristic;

namespace deskflow::ble {
class IBlePeripheralBackend;
}

namespace deskflow::ble {

class BleSocket;

// QObject host for Qt signal/slot wiring. Lives on the Qt thread that
// created the controller/service.
class BleSocketContext : public QObject
{
  Q_OBJECT
public:
  explicit BleSocketContext(BleSocket *owner);
  ~BleSocketContext() override;

  // Peripheral-side wiring: host uses `service` to receive upstream writes
  // from the remote and send notifications on the downstream characteristic.
  void attachPeripheral(QLowEnergyService *service, int mtu);

  // Peripheral-side wiring (backend-based): the BlePeripheralContext passes
  // its backend so this socket can forward outbound frames via
  // backend->sendDownstream. Inbound frames are pushed to deliverInbound()
  // by the context directly.
  void attachPeripheralBackend(deskflow::ble::IBlePeripheralBackend *backend, int mtu);

  // Central-side entry point. Called from BleSocket::connect() via queued
  // connection so all Qt Bluetooth work happens on the main thread.
  // `savedDeviceId` is the remembered remote UUID ("" to scan); `code` is
  // the 6-digit pairing code the user entered.
  Q_SLOT void startCentral(QString savedDeviceId, QString code);

  // Request disconnect / teardown.
  void detach();

  int mtu() const
  {
    return m_mtu;
  }
  void setMtu(int m)
  {
    m_mtu = m;
  }

  // Queue an outgoing frame for delivery on the Qt thread. Safe to call from
  // any thread.
  Q_SLOT void enqueueOutbound(QByteArray payload);

private Q_SLOTS:
  void onCharacteristicChanged(const QLowEnergyCharacteristic &ch, const QByteArray &value);
  void onScanDeviceDiscovered(const QBluetoothDeviceInfo &info);
  void onScanFinished();
  void onCentralConnected();
  void onCentralDisconnected();
  void onServiceDiscoveryFinished();
  void onServiceStateChanged(QLowEnergyService::ServiceState state);
  void onCharacteristicWritten(const QLowEnergyCharacteristic &ch, const QByteArray &value);

private:
  enum class Role
  {
    None,
    Peripheral,
    Central,
  };

  void connectToCentralDevice(const QBluetoothDeviceInfo &info);
  void enableNotifications();
  void writePairingCode();
  void drainCentralWriteQueue();
  void failCentral(const QString &reason);

  BleSocket *m_owner;
  QPointer<QLowEnergyService> m_service;
  deskflow::ble::IBlePeripheralBackend *m_peripheralBackend = nullptr;
  int m_mtu = 64; // default; updated by mtuChanged when negotiated
  // Per-direction packet-id source for outbound BLE chunks. Each
  // BleSocketContext is wired to exactly one outbound direction.
  BleFramingWriter m_framingWriter;

  // Central-mode state.
  Role m_role = Role::None;
  QBluetoothDeviceDiscoveryAgent *m_discovery = nullptr;
  QPointer<QLowEnergyController> m_centralCtl;
  QString m_pendingCode;
  QByteArray m_expectedHash; // 4-byte prefix
  bool m_pairingAccepted = false;
  QString m_savedDeviceId;
  QString m_connectedDeviceId;
  QQueue<QByteArray> m_centralWriteQueue;
  bool m_centralWriteInFlight = false;
  int m_scanSeen = 0;     // total devices scanned (any type)
  int m_scanLeSeen = 0;   // LE-capable devices
  int m_scanMagicHit = 0; // matched Deskflow magic (correct or wrong hash)
};

// BLE-backed IDataSocket. This version wires up the peripheral (host) side:
// reads from the upstream characteristic and writes to downstream notifies.
// Central-mode connect() is still stubbed and will post ConnectionFailed.
class BleSocket : public IDataSocket
{
public:
  explicit BleSocket(IEventQueue *events);
  ~BleSocket() override;

  // Adopt a peripheral-side GATT service as the transport. Called by
  // BleListenSocket once a remote has authenticated. Takes ownership of
  // `service` lifetime management via QPointer.
  void adoptPeripheralService(QLowEnergyService *service, int mtu);

  // Backend-based peripheral adoption (preferred going forward). Called by
  // BlePeripheralContext after successful pairing; the socket writes
  // outbound chunks through the backend and receives inbound data via
  // deliverInbound() pushed by the context.
  void adoptPeripheralBackend(deskflow::ble::IBlePeripheralBackend *backend, int mtu);

  // Feed a raw chunk received from the GATT layer into the framing reader
  // and publish any completed payloads as readable stream bytes. Safe to
  // call on the Qt thread that owns the service.
  void deliverInbound(const QByteArray &chunk);

  // Emit SocketDisconnected and clear buffers.
  void notifyDisconnected();

  // Mark a central-side BLE link as connected after pairing succeeds.
  void notifyConnected();

  // Update the MTU used by the framing writer. Safe to call from the Qt
  // thread when mtuChanged fires.
  void updateMtu(int mtu);

  // ISocket
  void bind(const NetworkAddress &) override;
  void close() override;
  void *getEventTarget() const override;

  // IDataSocket
  void connect(const NetworkAddress &) override;
  bool isFatal() const override;

  // IStream
  uint32_t read(void *buffer, uint32_t n) override;
  void write(const void *buffer, uint32_t n) override;
  void flush() override;
  void shutdownInput() override;
  void shutdownOutput() override;
  bool isReady() const override;
  uint32_t getSize() const override;

  IEventQueue *events() const
  {
    return m_events;
  }

private:
  void sendEvent(int type); // EventTypes cast to int to avoid header churn
  void scheduleInputReady();
  void emitDeferredInputReady();

  IEventQueue *m_events;
  BleSocketContext *m_ctx;

  mutable std::mutex m_mutex;
  QByteArray m_inputBuffer;
  BleFramingReader m_reader;
  bool m_inputShutdown = false;
  bool m_outputShutdown = false;
  bool m_connected = false;
  bool m_inputReadyScheduled = false;
};

} // namespace deskflow::ble
