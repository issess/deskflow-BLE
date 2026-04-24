/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BleFramingTests.h"

#include "ble/BleFraming.h"

using deskflow::ble::BleFramingReader;
using deskflow::ble::BleFramingWriter;

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
  auto chunks = BleFramingWriter::frame(payload, chunk);
  BleFramingReader r;
  for (const auto &c : chunks)
    r.feed(c);
  QByteArray out;
  r.next(out);
  return out;
}

} // namespace

void BleFramingTests::test_roundTrip_smallPayload()
{
  const QByteArray in = QByteArrayLiteral("hello");
  QCOMPARE(roundTrip(in, 20), in);
}

void BleFramingTests::test_roundTrip_boundaryMtu()
{
  // ATT MTU - 3 typical values.
  for (int chunk : {20, 100, 244, 512}) {
    // Exactly `chunk` bytes of payload — header + payload spans exactly two
    // chunks at chunk=20, more at others.
    const QByteArray in = makePayload(chunk);
    QCOMPARE(roundTrip(in, chunk), in);
  }
}

void BleFramingTests::test_roundTrip_largePayload()
{
  // 1 MiB across a small MTU.
  const QByteArray in = makePayload(1024 * 1024, 'Z');
  QCOMPARE(roundTrip(in, 247 - 3), in);
}

void BleFramingTests::test_writer_emptyPayload()
{
  auto chunks = BleFramingWriter::frame(QByteArray(), 20);
  // Just the 4-byte header.
  QCOMPARE(chunks.size(), std::size_t(1));
  QCOMPARE(chunks[0].size(), 4);
  BleFramingReader r;
  r.feed(chunks[0]);
  QByteArray out("sentinel");
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArray());
}

void BleFramingTests::test_writer_invalidChunkSize()
{
  auto chunks = BleFramingWriter::frame(QByteArray("abc"), 0);
  QVERIFY(chunks.empty());

  chunks = BleFramingWriter::frame(QByteArray("abc"), -5);
  QVERIFY(chunks.empty());
}

void BleFramingTests::test_reader_partialHeader()
{
  BleFramingReader r;
  QByteArray out;
  r.feed(QByteArray::fromHex("00"));
  QVERIFY(!r.next(out));
  r.feed(QByteArray::fromHex("00"));
  QVERIFY(!r.next(out));
  r.feed(QByteArray::fromHex("00"));
  QVERIFY(!r.next(out)); // still 3 of 4 header bytes
  r.feed(QByteArray::fromHex("03"));
  QVERIFY(!r.next(out)); // header complete, 0 of 3 payload
  r.feed(QByteArrayLiteral("xy"));
  QVERIFY(!r.next(out)); // 2 of 3 payload
  r.feed(QByteArrayLiteral("z"));
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArrayLiteral("xyz"));
}

void BleFramingTests::test_reader_multipleFramesInOneFeed()
{
  auto a = BleFramingWriter::frame(QByteArrayLiteral("alpha"), 64);
  auto b = BleFramingWriter::frame(QByteArrayLiteral("beta"), 64);
  BleFramingReader r;
  for (const auto &c : a)
    r.feed(c);
  for (const auto &c : b)
    r.feed(c);
  QByteArray first, second, third;
  QVERIFY(r.next(first));
  QVERIFY(r.next(second));
  QVERIFY(!r.next(third));
  QCOMPARE(first, QByteArrayLiteral("alpha"));
  QCOMPARE(second, QByteArrayLiteral("beta"));
}

void BleFramingTests::test_reader_reset()
{
  BleFramingReader r;
  r.feed(QByteArrayLiteral("\x00\x00\x00\x05he"));
  QCOMPARE(r.bufferedBytes(), std::size_t(6));
  r.reset();
  QCOMPARE(r.bufferedBytes(), std::size_t(0));

  auto c = BleFramingWriter::frame(QByteArrayLiteral("ok"), 64);
  for (const auto &x : c)
    r.feed(x);
  QByteArray out;
  QVERIFY(r.next(out));
  QCOMPARE(out, QByteArrayLiteral("ok"));
}

QTEST_MAIN(BleFramingTests)
