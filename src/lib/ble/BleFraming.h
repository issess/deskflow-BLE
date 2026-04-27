/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QQueue>
#include <cstdint>
#include <vector>

namespace deskflow::ble {

// Per-direction packet-ID framing for BLE GATT transport.
//
// Wire layout per BLE GATT chunk (one notify or one write):
//
//   offset 0  : u32 BE  packetId      // monotonic per direction; 0 reserved
//   offset 4  : u8      flags         // bit0=START, bit1=END
//   offset 5  : u8      chunkSeq      // 0..chunkCount-1
//   offset 6  : u8      chunkCount    // total chunks for this packetId; >=1
//   offset 7  : u8      reserved=0
//   offset 8  : u32 BE  payloadLen    // length of THIS chunk's payload bytes
//   offset 12 : bytes   payload
//
// A logical frame is the concatenation of payloads of all chunks sharing one
// packetId, in chunkSeq order. The transport above (PSF) is responsible for
// any length-prefixing inside the logical payload.
//
// The reader drops any chunk whose packetId is <= the last accepted id
// (stale / duplicate). If a gap is detected (id jumps by >1) any in-progress
// reassembly is discarded — losing a single logical frame instead of letting
// the upper-layer stream parser desync.
class BleFramingWriter
{
public:
  static constexpr int kHeaderSize = 12;
  static constexpr int kMaxChunksPerFrame = 255;

  struct Frame
  {
    quint32 packetId;
    std::vector<QByteArray> chunks;
  };

  // Build the chunks for one logical frame. maxChunk is the max BLE chunk
  // size in bytes (header + payload); typical value is the negotiated ATT
  // MTU minus 3. Bumps the internal packet-id counter on every call.
  // Returns Frame with empty chunks on invalid input (maxChunk <= header
  // size, or payload size requires >255 chunks at the given chunk size).
  Frame frame(const QByteArray &payload, int maxChunk);

  quint32 peekNextId() const
  {
    return m_nextId;
  }
  quint64 framesSent() const
  {
    return m_framesSent;
  }

  // Test hook: pre-set the next id (e.g. to exercise wraparound).
  void setNextId(quint32 id)
  {
    m_nextId = id == 0 ? 1 : id;
  }

private:
  quint32 m_nextId = 1; // 0 reserved as "never seen"
  quint64 m_framesSent = 0;
};

class BleFramingReader
{
public:
  enum class FeedResult
  {
    Accepted,     // chunk consumed and contributed to (or completed) a frame
    DroppedStale, // chunk's packetId <= last accepted id
    DroppedGap,   // header malformed, or in-progress frame had to be abandoned
    GapResync,    // accepted, but packetId jumped by >1 (frames lost)
  };

  // Push one BLE GATT chunk (one notify value or one write value) into the
  // reader. The result tells the caller whether anything was lost. On Accepted
  // / GapResync, complete logical frames may now be available via next().
  FeedResult feedChunk(const QByteArray &chunk);

  // Pop the next fully-assembled logical frame (via QQueue::dequeue). Returns
  // false when the queue is empty.
  bool next(QByteArray &outPayload);

  // Discard reader state (queued frames + in-progress assembly + last id).
  // Use on disconnect.
  void reset();

  // Diagnostics.
  quint32 lastAcceptedId() const
  {
    return m_lastAcceptedId;
  }
  std::size_t pendingAssemblyBytes() const
  {
    return static_cast<std::size_t>(m_assembly.size());
  }
  std::size_t readyFrames() const
  {
    return static_cast<std::size_t>(m_ready.size());
  }
  quint64 framesAccepted() const
  {
    return m_framesAccepted;
  }
  quint64 framesDroppedStale() const
  {
    return m_framesDroppedStale;
  }
  quint64 framesDroppedGap() const
  {
    return m_framesDroppedGap;
  }
  quint64 gapEvents() const
  {
    return m_gapEvents;
  }

private:
  void abandonAssembly();

  quint32 m_lastAcceptedId = 0;
  // In-progress reassembly state.
  bool m_assembling = false;
  quint32 m_currentId = 0;
  quint8 m_currentExpectSeq = 0;
  quint8 m_currentChunkCount = 0;
  QByteArray m_assembly;
  QQueue<QByteArray> m_ready;
  quint64 m_framesAccepted = 0;
  quint64 m_framesDroppedStale = 0;
  quint64 m_framesDroppedGap = 0;
  quint64 m_gapEvents = 0;
};

} // namespace deskflow::ble
