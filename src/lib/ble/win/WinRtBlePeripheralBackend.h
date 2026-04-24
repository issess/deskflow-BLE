/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ble/IBlePeripheralBackend.h"

#include <memory>

namespace deskflow::ble {

// Windows-only BLE peripheral backend using direct C++/WinRT calls.
// Bypasses Qt's QLowEnergyController peripheral path entirely because
// that path fails with "Error occurred trying to start advertising" on
// many desktop-class Bluetooth adapters (Broadcom, Realtek, ASUS USB
// dongles) even when the hardware and WinRT API support peripheral role.
//
// Uses the platform-preferred GattServiceProvider::StartAdvertising()
// overload which publishes both the service and the advert in one call.
class WinRtBlePeripheralBackend : public IBlePeripheralBackend
{
  Q_OBJECT
public:
  explicit WinRtBlePeripheralBackend(QObject *parent = nullptr);
  ~WinRtBlePeripheralBackend() override;

  bool start(const QString &localName, const QByteArray &manufacturerPayload) override;
  void stop() override;
  void sendPairingStatus(quint8 status) override;
  void sendDownstream(const QByteArray &chunk) override;
  int negotiatedMtu() const override;

private:
  // Pimpl to keep WinRT headers out of other translation units.
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow::ble
