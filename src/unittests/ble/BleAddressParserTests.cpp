/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BleAddressParserTests.h"

#include "ble/BleAddressParser.h"

using deskflow::ble::parseBleAddressString;

void BleAddressParserTests::test_parse_colonSeparated()
{
  // Canonical human-readable form, written into Deskflow.conf by users and
  // by the central backend after a successful pair (see BleSocket.cpp).
  QCOMPARE(parseBleAddressString(QStringLiteral("04:7F:0E:72:8E:39")),
           Q_UINT64_C(0x047F0E728E39));
}

void BleAddressParserTests::test_parse_dashSeparated()
{
  // Some BT tools (Linux bluetoothctl, hcitool) print dash-separated MACs.
  QCOMPARE(parseBleAddressString(QStringLiteral("04-7F-0E-72-8E-39")),
           Q_UINT64_C(0x047F0E728E39));
}

void BleAddressParserTests::test_parse_bareHex()
{
  // Bare-hex form is what scripts are most likely to produce.
  QCOMPARE(parseBleAddressString(QStringLiteral("047F0E728E39")),
           Q_UINT64_C(0x047F0E728E39));
}

void BleAddressParserTests::test_parse_caseInsensitive()
{
  // qstring->ulonglong tolerates either case; assert both work.
  QCOMPARE(parseBleAddressString(QStringLiteral("04:7f:0e:72:8e:39")),
           Q_UINT64_C(0x047F0E728E39));
  QCOMPARE(parseBleAddressString(QStringLiteral("04:7F:0E:72:8E:39")),
           Q_UINT64_C(0x047F0E728E39));
}

void BleAddressParserTests::test_parse_emptyReturnsZero()
{
  // Sentinel for "no direct address configured" — backend then falls back
  // to scan-based discovery.
  QCOMPARE(parseBleAddressString(QString()), Q_UINT64_C(0));
  QCOMPARE(parseBleAddressString(QStringLiteral("")), Q_UINT64_C(0));
}

void BleAddressParserTests::test_parse_tooShortReturnsZero()
{
  // Anything other than exactly 12 hex chars after stripping separators is
  // treated as malformed — same sentinel as empty input.
  QCOMPARE(parseBleAddressString(QStringLiteral("04:7F:0E:72:8E")), Q_UINT64_C(0));
  QCOMPARE(parseBleAddressString(QStringLiteral("12345")), Q_UINT64_C(0));
}

void BleAddressParserTests::test_parse_tooLongReturnsZero()
{
  QCOMPARE(parseBleAddressString(QStringLiteral("04:7F:0E:72:8E:39:00")), Q_UINT64_C(0));
  QCOMPARE(parseBleAddressString(QStringLiteral("0123456789ABCDEF")), Q_UINT64_C(0));
}

void BleAddressParserTests::test_parse_nonHexCharsReturnsZero()
{
  // 12 alphanumeric chars but non-hex → toULongLong fails, return 0.
  QCOMPARE(parseBleAddressString(QStringLiteral("ZZZZZZZZZZZZ")), Q_UINT64_C(0));
}

void BleAddressParserTests::test_parse_whitespaceTolerant()
{
  // Stray spaces from copy-paste should not break parsing — the parser
  // strips anything that isn't isLetterOrNumber().
  QCOMPARE(parseBleAddressString(QStringLiteral("  04:7F:0E:72:8E:39  ")),
           Q_UINT64_C(0x047F0E728E39));
}

QTEST_MAIN(BleAddressParserTests)
