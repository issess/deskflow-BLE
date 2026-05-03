/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QChar>
#include <QString>
#include <QtGlobal>

namespace deskflow::ble {

// Parse a 48-bit BT address from the human-friendly forms accepted by the
// Settings::Client::DirectBleAddress key — colon-separated (`"04:7F:0E:72:8E:39"`),
// dash-separated, or bare 12-hex-char (`"047F0E728E39"`), case-insensitive.
// Returns 0 for empty/malformed input — that's the same sentinel the central
// backend uses to fall back from direct-connect to scan-based discovery, so
// "rejected" inputs degrade gracefully without a separate error channel.
inline quint64 parseBleAddressString(const QString &raw)
{
  QString hex;
  hex.reserve(raw.size());
  for (QChar c : raw) {
    if (c.isLetterOrNumber())
      hex.append(c);
  }
  if (hex.size() != 12)
    return 0;
  bool ok = false;
  const quint64 v = hex.toULongLong(&ok, 16);
  return ok ? v : 0;
}

} // namespace deskflow::ble
