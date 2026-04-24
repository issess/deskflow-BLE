/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BleSocketFactory.h"

#include "base/Log.h"
#include "ble/BleListenSocket.h"
#include "ble/BleSocket.h"

namespace deskflow::ble {

BleSocketFactory::BleSocketFactory(IEventQueue *events) : m_events(events)
{
}

IDataSocket *BleSocketFactory::create(IArchNetwork::AddressFamily, SecurityLevel) const
{
  LOG_NOTE("BleSocketFactory::create (central BleSocket)");
  return new BleSocket(m_events);
}

IListenSocket *BleSocketFactory::createListen(IArchNetwork::AddressFamily, SecurityLevel) const
{
  LOG_NOTE("BleSocketFactory::createListen");
  return new BleListenSocket(m_events);
}

} // namespace deskflow::ble
