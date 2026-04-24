/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BlePairingCodeTests.h"

#include "ble/BlePairingCode.h"

#include <QSet>

using deskflow::ble::BlePairingCode;

void BlePairingCodeTests::test_generate_isSixDigits()
{
  BlePairingCode code;
  const QString s = code.generate();
  QCOMPARE(s.size(), 6);
  for (QChar c : s)
    QVERIFY(c.isDigit());
  QVERIFY(code.isSet());
}

void BlePairingCodeTests::test_generate_replacesPrevious()
{
  BlePairingCode code;
  const QString first = code.generate();
  // Statistically almost always different; repeat if unlucky.
  QString second = code.generate();
  for (int i = 0; i < 5 && second == first; ++i)
    second = code.generate();
  QVERIFY(second != first);
  QVERIFY(!code.verify(first));
  QVERIFY(code.verify(second));
}

void BlePairingCodeTests::test_verify_matchesCurrent()
{
  BlePairingCode code;
  const QString s = code.generate();
  QVERIFY(code.verify(s));
}

void BlePairingCodeTests::test_verify_rejectsMismatch()
{
  BlePairingCode code;
  code.generate();
  QVERIFY(!code.verify(QStringLiteral("000000")));
  QVERIFY(!code.verify(QStringLiteral("12345")));  // too short
  QVERIFY(!code.verify(QStringLiteral("1234567"))); // too long
  QVERIFY(!code.verify(QString()));
}

void BlePairingCodeTests::test_verify_rejectsAfterClear()
{
  BlePairingCode code;
  const QString s = code.generate();
  QVERIFY(code.verify(s));
  code.clear();
  QVERIFY(!code.isSet());
  QVERIFY(!code.verify(s));
}

void BlePairingCodeTests::test_hashPrefix_isStable()
{
  const QByteArray a = BlePairingCode::hashPrefix(QStringLiteral("123456"));
  const QByteArray b = BlePairingCode::hashPrefix(QStringLiteral("123456"));
  QCOMPARE(a, b);

  const QByteArray diff = BlePairingCode::hashPrefix(QStringLiteral("654321"));
  QVERIFY(a != diff);
}

void BlePairingCodeTests::test_hashPrefix_isFourBytes()
{
  QCOMPARE(BlePairingCode::hashPrefix(QStringLiteral("000000")).size(), 4);
  QCOMPARE(BlePairingCode::hashPrefix(QStringLiteral("999999")).size(), 4);
  QCOMPARE(BlePairingCode::hashPrefix(QString()).size(), 4);
}

void BlePairingCodeTests::test_generate_noTrivialCollisions()
{
  // 100 consecutive draws from 10^6 should yield at most a tiny handful of
  // duplicates. We assert < 5 to be robust against statistical flukes.
  BlePairingCode code;
  QSet<QString> seen;
  int collisions = 0;
  for (int i = 0; i < 100; ++i) {
    const QString s = code.generate();
    if (seen.contains(s))
      ++collisions;
    seen.insert(s);
  }
  QVERIFY2(collisions < 5, qPrintable(QStringLiteral("collisions=%1").arg(collisions)));
}

QTEST_MAIN(BlePairingCodeTests)
