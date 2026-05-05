/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/linux/BluezDbusTypes.h"

#include <QDBusMetaType>

namespace deskflow::ble::bluez {

void registerBluezDbusTypes()
{
  static bool s_registered = false;
  if (s_registered)
    return;
  s_registered = true;

  qDBusRegisterMetaType<PropertyMap>();
  qDBusRegisterMetaType<InterfaceMap>();
  qDBusRegisterMetaType<ManagedObjects>();
  qDBusRegisterMetaType<ManufacturerDataMap>();
}

} // namespace deskflow::ble::bluez
