/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <cstdint>
#include <vector>

namespace deskflow::ble {

// Length-prefixed framing for BLE GATT transport.
// Each logical message is serialized as [4-byte big-endian length][payload].
// The writer splits the framed message into chunks <= maxChunk to fit ATT MTU.
// The reader accumulates bytes from notify/write callbacks and yields complete
// payloads one at a time.
class BleFramingWriter
{
public:
  // Returns a sequence of chunks ready to send as single GATT writes/notifies.
  // maxChunk must be > 0; typical value is negotiated ATT MTU - 3.
  static std::vector<QByteArray> frame(const QByteArray &payload, int maxChunk);
};

class BleFramingReader
{
public:
  BleFramingReader() = default;

  // Feed raw bytes received from the GATT layer.
  void feed(const QByteArray &chunk);

  // Pop the next fully-received payload, or an empty QByteArray if none
  // is ready. Call in a loop until it returns empty.
  // Returns std::nullopt-equivalent via `ready` out param to distinguish
  // an empty-but-valid payload from "no frame yet".
  bool next(QByteArray &outPayload);

  // Reset reader buffer (e.g. on disconnect).
  void reset();

  // Expose current buffered bytes for diagnostics only.
  std::size_t bufferedBytes() const
  {
    return m_buffer.size();
  }

private:
  QByteArray m_buffer;
};

} // namespace deskflow::ble
