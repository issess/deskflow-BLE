/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BleFraming.h"

#include <QtEndian>
#include <cstring>

namespace deskflow::ble {

std::vector<QByteArray> BleFramingWriter::frame(const QByteArray &payload, int maxChunk)
{
  std::vector<QByteArray> out;
  if (maxChunk <= 0)
    return out;

  QByteArray framed;
  framed.reserve(4 + payload.size());
  quint32 len = static_cast<quint32>(payload.size());
  uchar hdr[4];
  qToBigEndian(len, hdr);
  framed.append(reinterpret_cast<const char *>(hdr), 4);
  framed.append(payload);

  for (int off = 0; off < framed.size(); off += maxChunk) {
    out.emplace_back(framed.mid(off, maxChunk));
  }
  return out;
}

void BleFramingReader::feed(const QByteArray &chunk)
{
  m_buffer.append(chunk);
}

bool BleFramingReader::next(QByteArray &outPayload)
{
  if (m_buffer.size() < 4)
    return false;
  quint32 len = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(m_buffer.constData()));
  if (m_buffer.size() < static_cast<int>(4 + len))
    return false;
  outPayload = m_buffer.mid(4, static_cast<int>(len));
  m_buffer.remove(0, 4 + len);
  return true;
}

void BleFramingReader::reset()
{
  m_buffer.clear();
}

} // namespace deskflow::ble
