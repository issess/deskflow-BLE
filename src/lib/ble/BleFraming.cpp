/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BleFraming.h"

#include <QtEndian>
#include <cstring>

namespace deskflow::ble {

namespace {

constexpr quint8 kFlagStart = 0x01;
constexpr quint8 kFlagEnd = 0x02;

void writeBe32(uchar *dst, quint32 v)
{
  dst[0] = static_cast<uchar>((v >> 24) & 0xFF);
  dst[1] = static_cast<uchar>((v >> 16) & 0xFF);
  dst[2] = static_cast<uchar>((v >> 8) & 0xFF);
  dst[3] = static_cast<uchar>(v & 0xFF);
}

quint32 readBe32(const uchar *src)
{
  return (static_cast<quint32>(src[0]) << 24) | (static_cast<quint32>(src[1]) << 16) |
         (static_cast<quint32>(src[2]) << 8) | static_cast<quint32>(src[3]);
}

} // namespace

BleFramingWriter::Frame BleFramingWriter::frame(const QByteArray &payload, int maxChunk)
{
  Frame out;
  if (maxChunk <= kHeaderSize)
    return out;

  const int maxPayloadPerChunk = maxChunk - kHeaderSize;
  const int payloadSize = payload.size();
  // chunkCount = ceil(payloadSize / maxPayloadPerChunk), but at least 1 chunk
  // even for an empty payload (so the receiver still observes a frame).
  int chunkCount = 1;
  if (payloadSize > 0) {
    chunkCount = (payloadSize + maxPayloadPerChunk - 1) / maxPayloadPerChunk;
  }
  if (chunkCount > kMaxChunksPerFrame)
    return out; // payload too large for the chunkCount field

  const quint32 id = m_nextId == 0 ? 1u : m_nextId;
  m_nextId = id + 1;
  if (m_nextId == 0)
    m_nextId = 1; // skip the reserved 0 on wrap

  out.packetId = id;
  out.chunks.reserve(static_cast<std::size_t>(chunkCount));

  for (int seq = 0; seq < chunkCount; ++seq) {
    const int off = seq * maxPayloadPerChunk;
    const int thisLen = std::min(maxPayloadPerChunk, payloadSize - off);
    QByteArray chunk;
    chunk.resize(kHeaderSize + thisLen);
    auto *p = reinterpret_cast<uchar *>(chunk.data());
    writeBe32(p, id);
    quint8 flags = 0;
    if (seq == 0)
      flags |= kFlagStart;
    if (seq == chunkCount - 1)
      flags |= kFlagEnd;
    p[4] = flags;
    p[5] = static_cast<quint8>(seq);
    p[6] = static_cast<quint8>(chunkCount);
    p[7] = 0;
    writeBe32(p + 8, static_cast<quint32>(thisLen));
    if (thisLen > 0) {
      std::memcpy(p + kHeaderSize, payload.constData() + off, static_cast<std::size_t>(thisLen));
    }
    out.chunks.emplace_back(std::move(chunk));
  }

  ++m_framesSent;
  return out;
}

void BleFramingReader::abandonAssembly()
{
  m_assembling = false;
  m_currentId = 0;
  m_currentExpectSeq = 0;
  m_currentChunkCount = 0;
  m_assembly.clear();
}

BleFramingReader::FeedResult BleFramingReader::feedChunk(const QByteArray &chunk)
{
  if (chunk.size() < BleFramingWriter::kHeaderSize) {
    ++m_framesDroppedGap;
    return FeedResult::DroppedGap;
  }

  const auto *p = reinterpret_cast<const uchar *>(chunk.constData());
  const quint32 packetId = readBe32(p);
  const quint8 flags = p[4];
  const quint8 chunkSeq = p[5];
  const quint8 chunkCount = p[6];
  // p[7] reserved
  const quint32 declaredLen = readBe32(p + 8);
  const int actualPayloadLen = chunk.size() - BleFramingWriter::kHeaderSize;

  // Sanity check: declared length must match actual chunk payload length and
  // chunkCount must be at least 1; chunkSeq must be inside the count.
  if (static_cast<quint32>(actualPayloadLen) != declaredLen || chunkCount == 0 || chunkSeq >= chunkCount) {
    ++m_framesDroppedGap;
    return FeedResult::DroppedGap;
  }
  // packetId 0 is reserved as "never seen" — refuse it on the wire.
  if (packetId == 0) {
    ++m_framesDroppedGap;
    return FeedResult::DroppedGap;
  }

  // Stale / duplicate detection: signed 32-bit modular comparison.
  // If we have never accepted anything, accept any non-zero id as the
  // baseline.
  bool gapDetected = false;
  if (m_lastAcceptedId != 0) {
    const qint32 diff = static_cast<qint32>(packetId - m_lastAcceptedId);
    if (diff <= 0) {
      ++m_framesDroppedStale;
      return FeedResult::DroppedStale;
    }
    if (diff > 1)
      gapDetected = true;
    // Count one gap event per frame, on the first chunk of a new packetId.
    // Subsequent chunks of the same multi-chunk frame must not double-count.
    if (gapDetected && !(m_assembling && m_currentId == packetId))
      ++m_gapEvents;
  }

  // Are we already assembling a different frame? Drop it — losing one frame
  // is preferable to letting the upper-layer stream parser desync.
  if (m_assembling && m_currentId != packetId) {
    abandonAssembly();
    ++m_framesDroppedGap;
    ++m_gapEvents;
  }

  // Start of a new frame: validate START flag and chunkSeq==0.
  if (!m_assembling) {
    if (chunkSeq != 0 || (flags & kFlagStart) == 0) {
      // Mid-frame chunk arrived but we have nothing to attach it to.
      ++m_framesDroppedGap;
      ++m_gapEvents;
      return FeedResult::DroppedGap;
    }
    m_assembling = true;
    m_currentId = packetId;
    m_currentExpectSeq = 0;
    m_currentChunkCount = chunkCount;
    m_assembly.clear();
    m_assembly.reserve(actualPayloadLen);
  } else {
    // Continuing an existing frame: chunkSeq must match expected.
    if (chunkSeq != m_currentExpectSeq || chunkCount != m_currentChunkCount) {
      abandonAssembly();
      ++m_framesDroppedGap;
      ++m_gapEvents;
      return FeedResult::DroppedGap;
    }
  }

  // Append payload.
  if (actualPayloadLen > 0) {
    m_assembly.append(chunk.constData() + BleFramingWriter::kHeaderSize, actualPayloadLen);
  }
  m_currentExpectSeq = static_cast<quint8>(chunkSeq + 1);

  const bool isEnd = (flags & kFlagEnd) != 0;
  // If END flag arrived but seq doesn't say we're done, treat as malformed.
  if (isEnd && (chunkSeq + 1) != chunkCount) {
    abandonAssembly();
    ++m_framesDroppedGap;
    ++m_gapEvents;
    return FeedResult::DroppedGap;
  }
  // If we've reached the end of the count without END flag, also malformed.
  if (!isEnd && (chunkSeq + 1) == chunkCount) {
    abandonAssembly();
    ++m_framesDroppedGap;
    ++m_gapEvents;
    return FeedResult::DroppedGap;
  }

  if (!isEnd) {
    return FeedResult::Accepted;
  }

  // Frame complete.
  m_ready.enqueue(m_assembly);
  m_lastAcceptedId = packetId;
  ++m_framesAccepted;
  abandonAssembly();
  return gapDetected ? FeedResult::GapResync : FeedResult::Accepted;
}

bool BleFramingReader::next(QByteArray &outPayload)
{
  if (m_ready.isEmpty())
    return false;
  outPayload = m_ready.dequeue();
  return true;
}

void BleFramingReader::reset()
{
  abandonAssembly();
  m_ready.clear();
  m_lastAcceptedId = 0;
  // Counters intentionally retained — they are session diagnostics.
}

} // namespace deskflow::ble
