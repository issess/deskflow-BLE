/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class BleProtocolClassifierTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void test_classifiesNoopPsfFrame();
  void test_keepAliveIsNotNoop();
  void test_classifiesMouseMoveBleChunk();
  void test_partialOrMultiChunkBleDataIsOther();
};
