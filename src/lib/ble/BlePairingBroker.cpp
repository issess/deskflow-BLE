/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BlePairingBroker.h"

namespace deskflow::ble {

BlePairingBroker &BlePairingBroker::instance()
{
  static BlePairingBroker s;
  return s;
}

void BlePairingBroker::setActiveCode(const QString &code)
{
  if (m_code == code)
    return;
  m_code = code;
  Q_EMIT codeChanged(m_code);
}

void BlePairingBroker::clearActiveCode()
{
  if (m_code.isEmpty())
    return;
  m_code.fill(QChar(u'\0'));
  m_code.clear();
  Q_EMIT codeChanged(QString());
}

void BlePairingBroker::reportResult(bool accepted, const QString &reason)
{
  Q_EMIT pairingResult(accepted, reason);
}

} // namespace deskflow::ble
