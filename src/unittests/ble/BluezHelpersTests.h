/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class BluezHelpersTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void test_pickAdapter_singlePowered();
  void test_pickAdapter_noneWhenNoAdapter();
  void test_pickAdapter_noneWhenAllUnpowered();
  void test_pickAdapter_lexicographicallyFirstWhenMultiplePowered();
  void test_pickAdapter_skipsNonAdapterObjects();
  void test_devicePath_canonicalForm();
  void test_devicePath_zeroPadsLowBytes();
};
