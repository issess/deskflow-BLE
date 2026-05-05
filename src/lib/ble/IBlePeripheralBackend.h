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

// Platform-agnostic interface for a BLE GATT peripheral that advertises
// the Deskflow service and relays PairingAuth writes / DataUpstream writes
// up to the pairing state machine, while letting that state machine push
// PairingStatus / DataDownstream notifies down.
//
// Two concrete implementations:
//   - QtBlePeripheralBackend    (Linux / macOS via QLowEnergyController)
//   - WinRtBlePeripheralBackend (Windows, direct C++/WinRT, bypasses
//                                 Qt's WinRT peripheral backend whose
//                                 StartAdvertising path is unreliable on
//                                 common Broadcom/Realtek USB adapters)
class IBlePeripheralBackend : public QObject
{
  Q_OBJECT
public:
  using QObject::QObject;
  ~IBlePeripheralBackend() override = default;

  // Build the GATT service, attach characteristics, and start advertising.
  // Returns false synchronously only for fatal pre-conditions (no adapter,
  // Bluetooth off). Async failures are delivered via the startFailed signal.
  virtual bool start(const QString &localName, const QByteArray &manufacturerPayload) = 0;

  // Tear down advertising and GATT server.
  virtual void stop() = 0;

  // Push a PairingStatus byte (Accepted / Rejected) to any subscribed peer.
  virtual void sendPairingStatus(quint8 status) = 0;

  // Push a DataDownstream chunk (already framed) to subscribed peer.
  virtual void sendDownstream(const QByteArray &chunk) = 0;

  // Negotiated ATT MTU (defaults to 64 until a peer-specific value is known).
  virtual int negotiatedMtu() const
  {
    return 64;
  }

  // Configure downstream (notify) reliability before start(). Default-true
  // backends (currently WinRT) submit each notify synchronously so a multi-
  // chunk frame can't be coalesced/dropped at the LL layer; lossy backends
  // fire-and-forget for higher throughput. Backends that can't honor the
  // requested mode (e.g. Qt peripheral has no GATT-level ack path) should
  // log once and run in their native mode.
  virtual void setDownstreamLossless(bool /*lossless*/)
  {
  }

Q_SIGNALS:
  // Advertising + service provider successfully published.
  void started();

  // Fatal error during setup; `reason` is user-facing.
  void startFailed(const QString &reason);

  // Central has written to PairingAuth. value is raw bytes written.
  void pairingAuthWritten(const QByteArray &value);

  // Central has written to DataUpstream. chunk is the raw bytes received.
  void upstreamWritten(const QByteArray &chunk);

  // Subscribed client events.
  void centralConnected();
  void centralDisconnected();

  // MTU negotiated (or updated).
  void mtuChanged(int mtu);
};

} // namespace deskflow::ble
