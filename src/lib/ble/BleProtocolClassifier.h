/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>

namespace deskflow::ble {

enum class BleProtocolFrameKind
{
  Other,
  Noop,
  MouseMove,
};

BleProtocolFrameKind classifyPsfFrame(const QByteArray &frame);
BleProtocolFrameKind classifyBleChunk(const QByteArray &chunk);

bool isNoopPsfFrame(const QByteArray &frame);
bool isMouseMoveBleChunk(const QByteArray &chunk);

} // namespace deskflow::ble
