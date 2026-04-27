/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BleProtocolClassifierTests.h"

#include "ble/BleProtocolClassifier.h"

#include <QtEndian>

using deskflow::ble::BleProtocolFrameKind;
using deskflow::ble::classifyBleChunk;
using deskflow::ble::classifyPsfFrame;
using deskflow::ble::isNoopPsfFrame;
using deskflow::ble::isMouseMoveBleChunk;

namespace {

QByteArray prefixLength(const QByteArray &payload)
{
  QByteArray out;
  uchar header[4];
  qToBigEndian(static_cast<quint32>(payload.size()), header);
  out.append(reinterpret_cast<const char *>(header), 4);
  out.append(payload);
  return out;
}

QByteArray psfFrame(const QByteArray &protocolPayload)
{
  return prefixLength(protocolPayload);
}

QByteArray bleChunkForPsfFrame(const QByteArray &frame)
{
  return prefixLength(frame);
}

} // namespace

void BleProtocolClassifierTests::test_classifiesNoopPsfFrame()
{
  const QByteArray frame = psfFrame(QByteArrayLiteral("CNOP"));
  QCOMPARE(classifyPsfFrame(frame), BleProtocolFrameKind::Noop);
  QVERIFY(isNoopPsfFrame(frame));
}

void BleProtocolClassifierTests::test_keepAliveIsNotNoop()
{
  const QByteArray frame = psfFrame(QByteArrayLiteral("CALV"));
  QCOMPARE(classifyPsfFrame(frame), BleProtocolFrameKind::Other);
  QVERIFY(!isNoopPsfFrame(frame));
}

void BleProtocolClassifierTests::test_classifiesMouseMoveBleChunk()
{
  const QByteArray absMove = bleChunkForPsfFrame(psfFrame(QByteArrayLiteral("DMMV\x00\x01\x00\x02")));
  const QByteArray relMove = bleChunkForPsfFrame(psfFrame(QByteArrayLiteral("DMRM\x00\x01\x00\x02")));

  QCOMPARE(classifyBleChunk(absMove), BleProtocolFrameKind::MouseMove);
  QCOMPARE(classifyBleChunk(relMove), BleProtocolFrameKind::MouseMove);
  QVERIFY(isMouseMoveBleChunk(absMove));
  QVERIFY(isMouseMoveBleChunk(relMove));
}

void BleProtocolClassifierTests::test_partialOrMultiChunkBleDataIsOther()
{
  const QByteArray full = bleChunkForPsfFrame(psfFrame(QByteArrayLiteral("DMMV\x00\x01\x00\x02")));
  QCOMPARE(classifyBleChunk(full.left(6)), BleProtocolFrameKind::Other);

  QByteArray multi = full;
  multi.append(bleChunkForPsfFrame(psfFrame(QByteArrayLiteral("DMMV\x00\x03\x00\x04"))));
  QCOMPARE(classifyBleChunk(multi), BleProtocolFrameKind::Other);
}

QTEST_MAIN(BleProtocolClassifierTests)
