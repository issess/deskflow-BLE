/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class BlePairingCodeTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void test_generate_isSixDigits();
  void test_generate_replacesPrevious();
  void test_verify_matchesCurrent();
  void test_verify_rejectsMismatch();
  void test_verify_rejectsAfterClear();
  void test_adopt_setsCurrent();
  void test_adopt_replacesGenerated();
  void test_adopt_emptyClears();
  void test_hashPrefix_isStable();
  void test_hashPrefix_isFourBytes();
  void test_generate_noTrivialCollisions();
};
