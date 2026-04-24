/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QString>

namespace deskflow::ble {

// Owns a 6-digit pairing code. Regenerated each session with a
// cryptographically secure RNG. `clear()` destroys the in-memory value.
class BlePairingCode
{
public:
  BlePairingCode() = default;

  // Generate a new 6-digit code, replacing any previous one.
  // Returns the code as a zero-padded 6-character string.
  QString generate();

  // Constant-time comparison against the currently held code.
  // Returns false if no code is currently held.
  bool verify(const QString &candidate) const;

  // Zero out the in-memory code.
  void clear();

  bool isSet() const;

  // SHA-256 prefix (first 4 bytes) — used in advertisement payload so
  // scanners can match a host without the code transiting the air.
  static QByteArray hashPrefix(const QString &code);

private:
  QString m_code;
};

} // namespace deskflow::ble
