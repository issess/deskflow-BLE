/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

namespace deskflow::ble {

// Platform-agnostic interface for a BLE GATT central that scans for the
// Deskflow service, connects to a host advertising it, and exposes the
// upstream-write / downstream-notify channels to BleSocket.
//
// Concrete implementations bypass Qt's QLowEnergyController on platforms
// where it serializes writes too aggressively or fails on common adapters:
//   - WinRtBleCentralBackend  (Windows, C++/WinRT)
//   - BluezBleCentralBackend  (Linux, direct org.bluez DBus)
//
// On platforms without a direct backend, BleSocketContext stays on its
// in-line Qt central path. The interface is therefore deliberately narrow
// and matches WinRtBleCentralBackend's public surface so promotion is
// mechanical.
class IBleCentralBackend : public QObject
{
  Q_OBJECT
public:
  using QObject::QObject;
  ~IBleCentralBackend() override = default;

  // Begin scan + connect + pair flow. `savedDeviceId` is empty for first-pair,
  // or a hex MAC for remembered-peer reconnect. `code` is the 6-digit pairing
  // code (empty when reconnecting a remembered peer). `directAddress`
  // (non-zero) bypasses the scan entirely and connects straight to that
  // 48-bit BT address.
  virtual void start(const QString &savedDeviceId, const QString &code, quint64 directAddress = 0) = 0;

  virtual void stop() = 0;

  // Submit one framed chunk for delivery on the DataUpstream characteristic.
  // Lossless mode: each chunk waits for an ACK before the next is sent.
  // Lossy mode: fire-and-forget WriteWithoutResponse for higher throughput.
  virtual void writeUpstream(const QByteArray &chunk) = 0;

  // Configure upstream reliability before start(). Backends that can't honor
  // the requested mode should log once and run in their native mode.
  virtual void setUpstreamLossless(bool /*lossless*/)
  {
  }

  virtual int mtu() const = 0;

  // 48-bit BT address of the currently/last-connected peer; 0 if never
  // connected. Suitable to persist as a remembered-peer hint for the next
  // session's reconnect.
  virtual quint64 peerAddress() const = 0;

Q_SIGNALS:
  void connected();                          // pairing accepted, link ready
  void disconnected();                       // peer disconnected
  void connectFailed(const QString &reason); // scan/connect/pair gave up
  void dataReceived(const QByteArray &data); // DataDownstream notify chunk
  void mtuChanged(int newMtu);
};

} // namespace deskflow::ble
