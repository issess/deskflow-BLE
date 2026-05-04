/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BleSecureListenSocket.h"

#include "ble/BleSecureSocket.h"
#include "ble/BleSocket.h"

namespace deskflow::ble {

BleSecureListenSocket::BleSecureListenSocket(IEventQueue *events, SecurityLevel level)
    : BleListenSocket(events),
      m_level(level)
{
}

std::unique_ptr<IDataSocket> BleSecureListenSocket::accept()
{
  auto inner = BleListenSocket::accept();
  if (!inner)
    return nullptr;
  return std::make_unique<BleSecureSocket>(events(), std::move(inner), m_level);
}

} // namespace deskflow::ble
