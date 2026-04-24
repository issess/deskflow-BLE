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
  void test_roundTrip_smallPayload();
  void test_roundTrip_boundaryMtu();
  void test_roundTrip_largePayload();
  void test_writer_emptyPayload();
  void test_writer_invalidChunkSize();
  void test_reader_partialHeader();
  void test_reader_multipleFramesInOneFeed();
  void test_reader_reset();
};
