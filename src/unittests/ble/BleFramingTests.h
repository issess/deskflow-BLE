/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class BleFramingTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // Existing round-trip behavior (now via instance writer + chunk reader).
  void test_roundTrip_smallPayload();
  void test_roundTrip_boundaryMtu();
  void test_roundTrip_largePayload();
  void test_writer_emptyPayload();
  void test_writer_invalidChunkSize();
  void test_reader_multipleFramesInOneFeed();
  void test_reader_reset();

  // Packet-id / gap behaviors added with the UDP-style refactor.
  void test_packetId_monotonic();
  void test_reader_dropsStale();
  void test_reader_dropsDuplicate();
  void test_reader_gapResync();
  void test_reader_partialFrameDiscardedOnNewId();
  void test_multiChunk_lostMiddle();
  void test_idWrap();
  void test_chunkCount_limitExceeded();
  void test_malformedHeader_dropped();
};
