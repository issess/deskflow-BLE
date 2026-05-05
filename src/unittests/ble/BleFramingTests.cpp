/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BleFramingTests.h"

#include "base/Log.h"
#include "ble/BleFraming.h"

#include <memory>

using deskflow::ble::BleFramingReader;
using deskflow::ble::BleFramingWriter;

namespace {
// Log singleton must exist before any LOG_* call (e.g. the DEBUG1 line in
// BleFramingReader::feedChunk). Allocate once for the whole test suite.
std::unique_ptr<Log> g_logInstance;
} // namespace

void BleFramingTests::initTestCase()
{
  if (!g_logInstance)
    g_logInstance = std::make_unique<Log>();
}

void BleFramingTests::cleanupTestCase()
{
  g_logInstance.reset();
}

namespace {

QByteArray makePayload(int size, char seed = 'A')
{
  QByteArray b(size, Qt::Uninitialized);
  for (int i = 0; i < size; ++i)
    b[i] = static_cast<char>(static_cast<unsigned char>(seed + (i % 251)));
  return b;
}

QByteArray roundTrip(const QByteArray &payload, int chunk)
{
  BleFramingWriter w;
  auto frame = w.frame(payload, chunk);
  BleFramingReader r;
  for (const auto &c : frame.chunks)
    r.feedChunk(c);
  QByteArray out;
  r.next(out);
  return out;
}

} // namespace

void BleFramingTests::test_roundTrip_smallPayload()
{
  const QByteArray in = QByteArrayLiteral("hello");
  // header is 12 bytes — chunk size must be > 12 to carry payload.
  QCOMPARE(roundTrip(in, 32), in);
}

void BleFramingTests::test_roundTrip_boundaryMtu()
{
  for (int chunk : {32, 64, 100, 244}) {
    const int maxPayload = chunk - BleFramingWriter::kHeaderSize;
    // Exactly maxPayload bytes — single chunk.
    const QByteArray in = makePayload(maxPayload);
    QCOMPARE(roundTrip(in, chunk), in);
    // One byte over — forces two chunks.
    const QByteArray in2 = makePayload(maxPayload + 1);
    QCOMPARE(roundTrip(in2, chunk), in2);
  }
}

void BleFramingTests::test_roundTrip_largePayload()
{
  // 64 KiB across an MTU-sized chunk. Stay below the 255-chunk cap.
  // 247-3 = 244 byte chunk → 244-12 = 232 payload bytes per chunk → ceil(65536/232) = 283 chunks.
  // Use a larger chunk size to keep within 255 chunks.
  // (244 - 12) * 255 = 59160 bytes max with 244-byte chunks; bump to 512.
  const QByteArray in = makePayload(64 * 1024, 'Z');
  QCOMPARE(roundTrip(in, 512), in);
}

void BleFramingTests::test_writer_emptyPayload()
{
  BleFramingWriter w;
  auto frame = w.frame(QByteArray(), 32);
  // Empty frame still emits one chunk so the receiver observes its existence.
  QCOMPARE(frame.chunks.size(), std::size_t(1));
  QCOMPARE(frame.chunks[0].size(), BleFramingWriter::kHeaderSize);

  BleFramingReader r;
  QCOMPARE(r.feedChunk(frame.chunks[0]), BleFramingReader::FeedResult::Accepted);
  QByteArray out("sentinel");
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArray());
}

void BleFramingTests::test_writer_invalidChunkSize()
{
  BleFramingWriter w;
  // chunk <= header size is rejected (no room for payload).
  QVERIFY(w.frame(QByteArray("abc"), 0).chunks.empty());
  QVERIFY(w.frame(QByteArray("abc"), -5).chunks.empty());
  QVERIFY(w.frame(QByteArray("abc"), BleFramingWriter::kHeaderSize).chunks.empty());
  // The id counter must not have moved.
  QCOMPARE(w.peekNextId(), quint32(1));
}

void BleFramingTests::test_reader_multipleFramesInOneFeed()
{
  BleFramingWriter w;
  auto a = w.frame(QByteArrayLiteral("alpha"), 64);
  auto b = w.frame(QByteArrayLiteral("beta"), 64);
  BleFramingReader r;
  for (const auto &c : a.chunks)
    r.feedChunk(c);
  for (const auto &c : b.chunks)
    r.feedChunk(c);
  QByteArray first, second, third;
  QVERIFY(r.next(first));
  QVERIFY(r.next(second));
  QVERIFY(!r.next(third));
  QCOMPARE(first, QByteArrayLiteral("alpha"));
  QCOMPARE(second, QByteArrayLiteral("beta"));
}

void BleFramingTests::test_reader_reset()
{
  BleFramingWriter w;
  BleFramingReader r;
  auto first = w.frame(QByteArrayLiteral("hello"), 64);
  for (const auto &c : first.chunks)
    r.feedChunk(c);

  // Reset wipes lastAcceptedId, so the next id (which is now smaller than
  // the just-reset baseline of 0) should be accepted as the new baseline.
  r.reset();
  QCOMPARE(r.lastAcceptedId(), quint32(0));

  auto second = w.frame(QByteArrayLiteral("ok"), 64);
  for (const auto &c : second.chunks)
    r.feedChunk(c);
  QByteArray out;
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArrayLiteral("ok"));
}

void BleFramingTests::test_packetId_monotonic()
{
  BleFramingWriter w;
  auto a = w.frame(QByteArrayLiteral("a"), 64);
  auto b = w.frame(QByteArrayLiteral("b"), 64);
  auto c = w.frame(QByteArrayLiteral("c"), 64);
  QCOMPARE(a.packetId, quint32(1));
  QCOMPARE(b.packetId, quint32(2));
  QCOMPARE(c.packetId, quint32(3));
  QCOMPARE(w.framesSent(), quint64(3));
}

void BleFramingTests::test_reader_dropsStale()
{
  BleFramingWriter w;
  BleFramingReader r;
  // Build five frames; feed only the 5th, then the 3rd. The 3rd is stale.
  std::vector<BleFramingWriter::Frame> frames;
  for (int i = 0; i < 5; ++i)
    frames.push_back(w.frame(QByteArrayLiteral("x"), 64));
  // Feed frame index 4 (id=5).
  for (const auto &c : frames[4].chunks)
    QCOMPARE(r.feedChunk(c), BleFramingReader::FeedResult::Accepted);
  // Now feed frame index 2 (id=3) — stale.
  for (const auto &c : frames[2].chunks) {
    QCOMPARE(r.feedChunk(c), BleFramingReader::FeedResult::DroppedStale);
  }
  QCOMPARE(r.lastAcceptedId(), quint32(5));
  QCOMPARE(r.framesDroppedStale(), quint64(1));
  // Only the first frame should be available.
  QByteArray out;
  QVERIFY(r.next(out));
  QVERIFY(!r.next(out));
}

void BleFramingTests::test_reader_dropsDuplicate()
{
  BleFramingWriter w;
  BleFramingReader r;
  auto a = w.frame(QByteArrayLiteral("hello"), 64);
  for (const auto &c : a.chunks)
    QCOMPARE(r.feedChunk(c), BleFramingReader::FeedResult::Accepted);
  // Feed the same chunks again — duplicate id, should be stale.
  for (const auto &c : a.chunks)
    QCOMPARE(r.feedChunk(c), BleFramingReader::FeedResult::DroppedStale);
  QCOMPARE(r.framesDroppedStale(), quint64(a.chunks.size()));
}

void BleFramingTests::test_reader_gapResync()
{
  BleFramingWriter w;
  BleFramingReader r;
  // Build ids 1..8.
  std::vector<BleFramingWriter::Frame> frames;
  for (int i = 0; i < 8; ++i)
    frames.push_back(w.frame(QByteArrayLiteral("p"), 64));
  // Feed id=5 (skipping 1..4).
  for (const auto &c : frames[4].chunks)
    QCOMPARE(r.feedChunk(c), BleFramingReader::FeedResult::Accepted);
  // Feed id=8 (skipping 6,7) — gap.
  // The single-chunk frame's last chunk returns GapResync.
  for (const auto &c : frames[7].chunks)
    QCOMPARE(r.feedChunk(c), BleFramingReader::FeedResult::GapResync);
  QCOMPARE(r.gapEvents(), quint64(1));
  QCOMPARE(r.lastAcceptedId(), quint32(8));
  QByteArray out;
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArrayLiteral("p"));
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArrayLiteral("p"));
  QVERIFY(!r.next(out));
}

void BleFramingTests::test_reader_partialFrameDiscardedOnNewId()
{
  BleFramingWriter w;
  // Force multi-chunk: payload bigger than maxPayloadPerChunk = 32-12 = 20.
  const QByteArray big = makePayload(40, 'M'); // ceil(40/20) = 2 chunks
  auto a = w.frame(big, 32);
  QCOMPARE(a.chunks.size(), std::size_t(2));
  // Single-chunk follow-up.
  auto b = w.frame(QByteArrayLiteral("next"), 64);

  BleFramingReader r;
  // Feed only the first chunk of `a` (START, not END).
  QCOMPARE(r.feedChunk(a.chunks[0]), BleFramingReader::FeedResult::Accepted);
  QVERIFY(r.pendingAssemblyBytes() > 0);
  // Now feed `b` — id changes mid-assembly. `a` is dropped, `b` accepted.
  // The first chunk of `b` is its only chunk (START|END), so we get GapResync
  // because id jumped from a.id to b.id (diff > 1 once we account for the
  // abandoned frame, but since lastAcceptedId is still 0 here we just accept).
  // The key invariant is: after the new id arrives, the abandoned partial is
  // counted in framesDroppedGap and gapEvents.
  for (const auto &c : b.chunks) {
    auto res = r.feedChunk(c);
    QVERIFY(res == BleFramingReader::FeedResult::Accepted ||
            res == BleFramingReader::FeedResult::GapResync);
  }
  QCOMPARE(r.framesDroppedGap(), quint64(1));
  QCOMPARE(r.gapEvents(), quint64(1));
  QByteArray out;
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArrayLiteral("next"));
  QVERIFY(!r.next(out));
}

void BleFramingTests::test_multiChunk_lostMiddle()
{
  BleFramingWriter w;
  // chunkSize 32, payload 60 → 3 chunks (20 + 20 + 20).
  const QByteArray payload = makePayload(60, 'X');
  auto frame = w.frame(payload, 32);
  QCOMPARE(frame.chunks.size(), std::size_t(3));

  BleFramingReader r;
  QCOMPARE(r.feedChunk(frame.chunks[0]), BleFramingReader::FeedResult::Accepted);
  // Skip chunks[1], feed chunks[2]. Reader sees chunkSeq=2 but expects 1 →
  // DroppedGap, in-progress assembly thrown out.
  QCOMPARE(r.feedChunk(frame.chunks[2]), BleFramingReader::FeedResult::DroppedGap);
  QByteArray out;
  QVERIFY(!r.next(out));
  QCOMPARE(r.framesDroppedGap(), quint64(1));
}

void BleFramingTests::test_idWrap()
{
  BleFramingWriter w;
  w.setNextId(0xFFFFFFFEu);
  auto a = w.frame(QByteArrayLiteral("near-end"), 64);
  auto b = w.frame(QByteArrayLiteral("at-end"), 64);
  auto c = w.frame(QByteArrayLiteral("after-wrap"), 64);
  QCOMPARE(a.packetId, quint32(0xFFFFFFFEu));
  QCOMPARE(b.packetId, quint32(0xFFFFFFFFu));
  // 0 is reserved → wrap skips to 1.
  QCOMPARE(c.packetId, quint32(1));

  BleFramingReader r;
  for (const auto &x : a.chunks)
    QCOMPARE(r.feedChunk(x), BleFramingReader::FeedResult::Accepted);
  for (const auto &x : b.chunks)
    QCOMPARE(r.feedChunk(x), BleFramingReader::FeedResult::Accepted);
  // Across the wrap: signed-32-bit modular comparison sees diff =
  // int32_t(1 - 0xFFFFFFFF) = +2 → accepted (with gap because diff > 1).
  for (const auto &x : c.chunks)
    QCOMPARE(r.feedChunk(x), BleFramingReader::FeedResult::GapResync);
  QByteArray out;
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArrayLiteral("near-end"));
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArrayLiteral("at-end"));
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArrayLiteral("after-wrap"));
}

void BleFramingTests::test_chunkCount_limitExceeded()
{
  // 255 chunks * (64-12) bytes = 13260. Try a payload too large to fit.
  BleFramingWriter w;
  const QByteArray huge = makePayload(64 * 256, 'Q'); // way over 255 * 52
  auto frame = w.frame(huge, 64);
  QVERIFY(frame.chunks.empty());
  // Counter shouldn't have advanced on rejection.
  QCOMPARE(w.peekNextId(), quint32(1));
}

void BleFramingTests::test_malformedHeader_dropped()
{
  BleFramingReader r;
  // Header smaller than 12 bytes.
  QCOMPARE(r.feedChunk(QByteArray(5, '\0')), BleFramingReader::FeedResult::DroppedGap);
  QCOMPARE(r.framesDroppedGap(), quint64(1));

  // Reserved-id 0 on the wire.
  QByteArray bad(BleFramingWriter::kHeaderSize, '\0');
  // packetId=0 already; declaredLen=0, chunkCount=0 — multiple things wrong.
  QCOMPARE(r.feedChunk(bad), BleFramingReader::FeedResult::DroppedGap);
}

QTEST_MAIN(BleFramingTests)
