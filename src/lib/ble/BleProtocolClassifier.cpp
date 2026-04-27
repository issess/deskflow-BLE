/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BleProtocolClassifier.h"

#include <QtEndian>
#include <cstring>

namespace deskflow::ble {

namespace {

bool codeEquals(const char *actual, const char *expected)
{
  return std::memcmp(actual, expected, 4) == 0;
}

BleProtocolFrameKind classifyProtocolPayload(const QByteArray &payload)
{
  if (payload.size() < 4)
    return BleProtocolFrameKind::Other;

  const char *code = payload.constData();
  if (codeEquals(code, "CNOP"))
    return BleProtocolFrameKind::Noop;
  if (codeEquals(code, "DMMV") || codeEquals(code, "DMRM"))
    return BleProtocolFrameKind::MouseMove;
  return BleProtocolFrameKind::Other;
}

} // namespace

BleProtocolFrameKind classifyPsfFrame(const QByteArray &frame)
{
  if (frame.size() < 8)
    return BleProtocolFrameKind::Other;

  const auto payloadSize = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(frame.constData()));
  if (payloadSize > static_cast<quint32>(frame.size() - 4))
    return BleProtocolFrameKind::Other;
  if (static_cast<int>(payloadSize) != frame.size() - 4)
    return BleProtocolFrameKind::Other;

  return classifyProtocolPayload(frame.mid(4, static_cast<int>(payloadSize)));
}

BleProtocolFrameKind classifyBleChunk(const QByteArray &chunk)
{
  if (chunk.size() < 8)
    return BleProtocolFrameKind::Other;

  const auto payloadSize = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(chunk.constData()));
  if (payloadSize > static_cast<quint32>(chunk.size() - 4))
    return BleProtocolFrameKind::Other;
  if (static_cast<int>(payloadSize) != chunk.size() - 4)
    return BleProtocolFrameKind::Other;

  return classifyPsfFrame(chunk.mid(4, static_cast<int>(payloadSize)));
}

bool isNoopPsfFrame(const QByteArray &frame)
{
  return classifyPsfFrame(frame) == BleProtocolFrameKind::Noop;
}

bool isMouseMoveBleChunk(const QByteArray &chunk)
{
  return classifyBleChunk(chunk) == BleProtocolFrameKind::MouseMove;
}

} // namespace deskflow::ble
