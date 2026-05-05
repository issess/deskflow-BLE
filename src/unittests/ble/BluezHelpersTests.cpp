/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BluezHelpersTests.h"

#include "ble/linux/BluezAdapter.h"
#include "ble/linux/BluezBleCentralBackend.h"
#include "ble/linux/BluezDbusTypes.h"

using namespace deskflow::ble;
using namespace deskflow::ble::bluez;

namespace {

InterfaceMap adapter1(bool powered)
{
  InterfaceMap ifaces;
  PropertyMap props;
  props.insert(QStringLiteral("Powered"), powered);
  ifaces.insert(QStringLiteral("org.bluez.Adapter1"), props);
  return ifaces;
}

InterfaceMap nonAdapter()
{
  InterfaceMap ifaces;
  PropertyMap props;
  props.insert(QStringLiteral("Address"), QStringLiteral("00:11:22:33:44:55"));
  ifaces.insert(QStringLiteral("org.bluez.Device1"), props);
  return ifaces;
}

} // namespace

void BluezHelpersTests::test_pickAdapter_singlePowered()
{
  ManagedObjects objs;
  objs.insert(QDBusObjectPath(QStringLiteral("/org/bluez/hci0")), adapter1(true));
  QCOMPARE(pickAdapter(objs).path(), QStringLiteral("/org/bluez/hci0"));
}

void BluezHelpersTests::test_pickAdapter_noneWhenNoAdapter()
{
  ManagedObjects objs;
  objs.insert(QDBusObjectPath(QStringLiteral("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF")), nonAdapter());
  // No Adapter1 — fall through to empty path.
  QVERIFY(pickAdapter(objs).path().isEmpty());
}

void BluezHelpersTests::test_pickAdapter_noneWhenAllUnpowered()
{
  ManagedObjects objs;
  objs.insert(QDBusObjectPath(QStringLiteral("/org/bluez/hci0")), adapter1(false));
  objs.insert(QDBusObjectPath(QStringLiteral("/org/bluez/hci1")), adapter1(false));
  // Adapter present but powered off — distinguish from "no adapter" upstream.
  // Helper just returns empty; the message disambiguation happens in start().
  QVERIFY(pickAdapter(objs).path().isEmpty());
}

void BluezHelpersTests::test_pickAdapter_lexicographicallyFirstWhenMultiplePowered()
{
  // Multi-controller hosts: the choice must be deterministic so the user
  // doesn't see the BLE service migrate between adapters across restarts.
  // Lexical sort matches what the legacy Qt path does (first hci device).
  ManagedObjects objs;
  objs.insert(QDBusObjectPath(QStringLiteral("/org/bluez/hci2")), adapter1(true));
  objs.insert(QDBusObjectPath(QStringLiteral("/org/bluez/hci0")), adapter1(true));
  objs.insert(QDBusObjectPath(QStringLiteral("/org/bluez/hci1")), adapter1(true));
  QCOMPARE(pickAdapter(objs).path(), QStringLiteral("/org/bluez/hci0"));
}

void BluezHelpersTests::test_pickAdapter_skipsNonAdapterObjects()
{
  // Realistic snapshot: one adapter plus a few cached devices. The function
  // must look only at Adapter1 interfaces, not get confused by Device1
  // entries that happen to have a "Powered" key.
  ManagedObjects objs;
  objs.insert(QDBusObjectPath(QStringLiteral("/org/bluez/hci0")), adapter1(true));
  objs.insert(QDBusObjectPath(QStringLiteral("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF")), nonAdapter());
  QCOMPARE(pickAdapter(objs).path(), QStringLiteral("/org/bluez/hci0"));
}

void BluezHelpersTests::test_devicePath_canonicalForm()
{
  // BlueZ's device path convention is exactly /<adapter>/dev_AA_BB_CC_DD_EE_FF
  // with upper-case hex pairs joined by underscores. Mismatching casing
  // ("dev_aa_bb_...") makes Device1.Connect raise UnknownObject.
  const auto p = BluezBleCentralBackend::deviceObjectPathFromAddress(
      QStringLiteral("/org/bluez/hci0"), Q_UINT64_C(0x047F0E728E39));
  QCOMPARE(p, QStringLiteral("/org/bluez/hci0/dev_04_7F_0E_72_8E_39"));
}

void BluezHelpersTests::test_devicePath_zeroPadsLowBytes()
{
  // A zero in the middle byte must still be rendered as "00", not elided.
  const auto p = BluezBleCentralBackend::deviceObjectPathFromAddress(
      QStringLiteral("/org/bluez/hci0"), Q_UINT64_C(0x010002000300));
  QCOMPARE(p, QStringLiteral("/org/bluez/hci0/dev_01_00_02_00_03_00"));
}

QTEST_MAIN(BluezHelpersTests)
