/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BlePairingCode.h"

#include "ble/BleTransport.h"

#include <QCryptographicHash>
#include <QRandomGenerator>

namespace deskflow::ble {

QString BlePairingCode::generate()
{
  // 0 .. 999999, zero-padded. QRandomGenerator::system() pulls from the OS CSPRNG.
  const quint32 n = QRandomGenerator::system()->bounded(1'000'000u);
  m_code = QStringLiteral("%1").arg(n, kPairingCodeDigits, 10, QLatin1Char('0'));
  return m_code;
}

void BlePairingCode::adopt(const QString &code)
{
  m_code = code;
}

bool BlePairingCode::verify(const QString &candidate) const
{
  if (m_code.isEmpty() || candidate.size() != m_code.size())
    return false;
  // Constant-time compare.
  quint32 diff = 0;
  for (int i = 0; i < m_code.size(); ++i) {
    diff |= static_cast<quint32>(m_code.at(i).unicode()) ^ static_cast<quint32>(candidate.at(i).unicode());
  }
  return diff == 0;
}

void BlePairingCode::clear()
{
  m_code.fill(QChar(u'\0'));
  m_code.clear();
}

bool BlePairingCode::isSet() const
{
  return !m_code.isEmpty();
}

QByteArray BlePairingCode::hashPrefix(const QString &code)
{
  const QByteArray full = QCryptographicHash::hash(code.toUtf8(), QCryptographicHash::Sha256);
  return full.left(4);
}

} // namespace deskflow::ble
