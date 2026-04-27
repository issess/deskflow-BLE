/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ble/BlePairingCode.h"
#include "net/IListenSocket.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <deque>
#include <memory>
#include <mutex>

class IEventQueue;
class QTimer;

namespace deskflow::ble {

class BleListenSocket;
class BleSocket;
class IBlePeripheralBackend;

// Pairing state machine that sits on top of a platform-specific peripheral
// backend (Qt QLowEnergyController on Linux, direct C++/WinRT on Windows).
// Owns the 6-digit code lifecycle, attempt counter, timeout, and hands an
// accepted BleSocket to the listener once the remote presents the correct
// code.
class BlePeripheralContext : public QObject
{
  Q_OBJECT
public:
  explicit BlePeripheralContext(BleListenSocket *owner);
  ~BlePeripheralContext() override;

  bool start();
  void stop();

  QString currentCode() const
  {
    return m_currentCode;
  }

private Q_SLOTS:
  void onBackendStarted();
  void onBackendStartFailed(const QString &reason);
  void onPairingAuthWritten(const QByteArray &value);
  void onUpstreamWritten(const QByteArray &value);
  void onCentralConnected();
  void onCentralDisconnected();
  void onMtuChanged(int mtu);
  void onTimeout();

private:
  void publishCode();
  void resetCode();
  void tearDown();
  QByteArray buildManufacturerData() const;

  BleListenSocket *m_owner;
  IBlePeripheralBackend *m_backend = nullptr;
  QTimer *m_timeout = nullptr;
  BlePairingCode m_code;
  QString m_currentCode;
  int m_attempts = 0;
  bool m_paired = false;
  BleSocket *m_acceptedSocket = nullptr;
  // 0 means "not yet negotiated"; adoptPeripheralBackend then falls back to
  // the modern BLE default (247) instead of the legacy 23-byte ATT MTU.
  int m_negotiatedMtu = 0;
};

// Peripheral-mode listen socket.
class BleListenSocket : public IListenSocket
{
public:
  explicit BleListenSocket(IEventQueue *events);
  ~BleListenSocket() override;

  // ISocket
  void bind(const NetworkAddress &) override;
  void close() override;
  void *getEventTarget() const override;

  // IListenSocket
  std::unique_ptr<IDataSocket> accept() override;

  // Called by BlePeripheralContext once a remote has authenticated.
  void pushAcceptedSocket(std::unique_ptr<BleSocket> socket);

  IEventQueue *events() const
  {
    return m_events;
  }

private:
  IEventQueue *m_events;
  BlePeripheralContext *m_ctx;

  std::mutex m_mutex;
  std::deque<std::unique_ptr<BleSocket>> m_ready;
};

} // namespace deskflow::ble
