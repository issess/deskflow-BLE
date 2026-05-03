/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class BleAddressParserTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void test_parse_colonSeparated();
  void test_parse_dashSeparated();
  void test_parse_bareHex();
  void test_parse_caseInsensitive();
  void test_parse_emptyReturnsZero();
  void test_parse_tooShortReturnsZero();
  void test_parse_tooLongReturnsZero();
  void test_parse_nonHexCharsReturnsZero();
  void test_parse_whitespaceTolerant();
};
