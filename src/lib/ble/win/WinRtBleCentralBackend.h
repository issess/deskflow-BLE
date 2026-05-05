// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
//
// Windows-only BLE central backend. Replaces Qt's QLowEnergyController on
// Windows so the upstream-write path can call WinRT GattCharacteristic
// .WriteValueAsync as truly fire-and-forget, instead of going through Qt's
// internal serialisation that caps writeCharacteristic at ~16 ms per call
// (~2 KB/s upstream).

#pragma once

#include "ble/IBleCentralBackend.h"

#include <QByteArray>
#include <QString>
#include <memory>

namespace deskflow::ble {

class WinRtBleCentralBackend : public IBleCentralBackend
{
  Q_OBJECT
public:
  explicit WinRtBleCentralBackend(QObject *parent = nullptr);
  ~WinRtBleCentralBackend() override;

  void start(const QString &savedDeviceId, const QString &code, quint64 directAddress = 0) override;
  void stop() override;

  // Truly fire-and-forget upstream write. Each chunk is one ATT PDU.
  void writeUpstream(const QByteArray &chunk) override;

  // Configure upstream reliability. Default true: WriteWithResponse + .get()
  // (per-chunk ATT-acked, lossless, ~one connection-event of latency). False:
  // WriteWithoutResponse fire-and-forget (higher throughput, accepts BleFraming
  // drop-and-resync). Must be called before the first writeUpstream().
  void setUpstreamLossless(bool lossless) override;

  int mtu() const override;

  // 48-bit BT address of the currently/last-connected peer; 0 if never
  // connected. The host adapter's public MAC, suitable to persist as a
  // remembered-peer hint for the next session's reconnect.
  quint64 peerAddress() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow::ble
