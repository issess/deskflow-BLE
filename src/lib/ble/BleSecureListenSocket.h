/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ble/BleListenSocket.h"
#include "net/SecurityLevel.h"

namespace deskflow::ble {

// Listen socket that returns BleSecureSocket instances on accept(). Inherits
// the bind/close/event-target wiring from BleListenSocket so the listener
// behaves identically at the BLE layer; only the data sockets handed back
// after pairing get wrapped in TLS.
class BleSecureListenSocket : public BleListenSocket
{
public:
  BleSecureListenSocket(IEventQueue *events, SecurityLevel level);
  ~BleSecureListenSocket() override = default;

  std::unique_ptr<IDataSocket> accept() override;

private:
  SecurityLevel m_level;
};

} // namespace deskflow::ble
