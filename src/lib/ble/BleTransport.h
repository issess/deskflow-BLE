/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QBluetoothUuid>

namespace deskflow::ble {

// Deskflow BLE GATT service and characteristics.
// UUIDs are v4 randoms dedicated to deskflow; never reuse outside this project.
inline const QBluetoothUuid kServiceUuid{QStringLiteral("7d8a4b5e-6c1f-4a2d-9b3e-8f0a1c2d3e40")};
inline const QBluetoothUuid kPairingAuthCharUuid{QStringLiteral("7d8a4b5e-6c1f-4a2d-9b3e-8f0a1c2d3e41")};
inline const QBluetoothUuid kPairingStatusCharUuid{QStringLiteral("7d8a4b5e-6c1f-4a2d-9b3e-8f0a1c2d3e42")};
inline const QBluetoothUuid kDataDownstreamCharUuid{QStringLiteral("7d8a4b5e-6c1f-4a2d-9b3e-8f0a1c2d3e43")};
inline const QBluetoothUuid kDataUpstreamCharUuid{QStringLiteral("7d8a4b5e-6c1f-4a2d-9b3e-8f0a1c2d3e44")};
inline const QBluetoothUuid kControlCharUuid{QStringLiteral("7d8a4b5e-6c1f-4a2d-9b3e-8f0a1c2d3e45")};

// Manufacturer data: 2-byte magic + 4-byte SHA-256 prefix of pairing code.
constexpr quint16 kManufacturerId = 0xFFFF; // testing / not-assigned Bluetooth SIG range
constexpr quint16 kAdvertMagic = 0xD5F1;    // "DesFlow"

constexpr int kPairingCodeDigits = 6;
constexpr int kPairingTimeoutSeconds = 120;
constexpr int kPairingMaxAttempts = 3;

enum class PairingStatus : quint8
{
  Pending = 0,
  Accepted = 1,
  Rejected = 2,
  TimedOut = 3,
};

} // namespace deskflow::ble
