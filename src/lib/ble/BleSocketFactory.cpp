/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BleSocketFactory.h"

#include "base/Log.h"
#include "ble/BleListenSocket.h"
#include "ble/BleSecureListenSocket.h"
#include "ble/BleSecureSocket.h"
#include "ble/BleSocket.h"

namespace deskflow::ble {

BleSocketFactory::BleSocketFactory(IEventQueue *events) : m_events(events)
{
}

IDataSocket *BleSocketFactory::create(IArchNetwork::AddressFamily, SecurityLevel level) const
{
  if (level != SecurityLevel::PlainText) {
    LOG_NOTE("BleSocketFactory::create (BleSecureSocket level=%d)", static_cast<int>(level));
    return new BleSecureSocket(m_events, level);
  }
  LOG_NOTE("BleSocketFactory::create (BleSocket plaintext)");
  return new BleSocket(m_events);
}

IListenSocket *BleSocketFactory::createListen(IArchNetwork::AddressFamily, SecurityLevel level) const
{
  if (level != SecurityLevel::PlainText) {
    LOG_NOTE("BleSocketFactory::createListen (BleSecureListenSocket level=%d)", static_cast<int>(level));
    return new BleSecureListenSocket(m_events, level);
  }
  LOG_NOTE("BleSocketFactory::createListen (BleListenSocket plaintext)");
  return new BleListenSocket(m_events);
}

} // namespace deskflow::ble
