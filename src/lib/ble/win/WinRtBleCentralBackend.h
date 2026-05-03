// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
//
// Windows-only BLE central backend. Replaces Qt's QLowEnergyController on
// Windows so the upstream-write path can call WinRT GattCharacteristic
// .WriteValueAsync as truly fire-and-forget, instead of going through Qt's
// internal serialisation that caps writeCharacteristic at ~16 ms per call
// (~2 KB/s upstream).

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <memory>

namespace deskflow::ble {

class WinRtBleCentralBackend : public QObject
{
  Q_OBJECT
public:
  explicit WinRtBleCentralBackend(QObject *parent = nullptr);
  ~WinRtBleCentralBackend() override;

  // Begin scan + connect + pair flow. `savedDeviceId` is empty for first-pair,
  // or a hex MAC for remembered-peer reconnect. `code` is the 6-digit pairing
  // code (empty when reconnecting a remembered peer).
  // `directAddress` (optional, non-zero) bypasses the scan entirely and connects
  // straight to that 48-bit BT address — for adapters whose peripheral-mode
  // advertising can't be discovered via scan but can still accept a direct
  // GATT connection by address.
  void start(const QString &savedDeviceId, const QString &code, quint64 directAddress = 0);

  void stop();

  // Truly fire-and-forget upstream write. Each chunk is one ATT PDU.
  void writeUpstream(const QByteArray &chunk);

  int mtu() const;

  // 48-bit BT address of the currently/last-connected peer; 0 if never
  // connected. The host adapter's public MAC, suitable to persist as a
  // remembered-peer hint for the next session's reconnect.
  quint64 peerAddress() const;

Q_SIGNALS:
  void connected();                          // pairing accepted, link ready
  void disconnected();                       // peer disconnected
  void connectFailed(const QString &reason); // scan/connect/pair gave up
  void dataReceived(const QByteArray &data); // DataDownstream notify chunk
  void mtuChanged(int newMtu);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow::ble
