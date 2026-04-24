/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/IArchNetwork.h"
#include "net/ISocketFactory.h"

class IEventQueue;

namespace deskflow::ble {

class BleSocketFactory : public ISocketFactory
{
public:
  explicit BleSocketFactory(IEventQueue *events);
  ~BleSocketFactory() override = default;

  IDataSocket *create(
      IArchNetwork::AddressFamily family = IArchNetwork::AddressFamily::INet,
      SecurityLevel securityLevel = SecurityLevel::PlainText
  ) const override;
  IListenSocket *createListen(
      IArchNetwork::AddressFamily family = IArchNetwork::AddressFamily::INet,
      SecurityLevel securityLevel = SecurityLevel::PlainText
  ) const override;

private:
  IEventQueue *m_events;
};

} // namespace deskflow::ble
