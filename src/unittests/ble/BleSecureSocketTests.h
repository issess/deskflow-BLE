/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QString>
#include <QTest>
#include <memory>

class BleSecureSocketTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();

  // Wrapper plumbing — no SSL traffic.
  void test_construct_serverSide_validCert();
  void test_construct_serverSide_missingCert_emitsFail();
  void test_close_idempotent();
  void test_read_beforeHandshake_returnsZero();
  void test_isReady_beforeHandshake_false();
  void test_getSize_beforeHandshake_zero();
  void test_getEventTarget_stable();

  // End-to-end SSL via in-memory loopback transport.
  void test_handshake_encrypted_completes();
  void test_dataRoundTrip_encrypted();
  void test_dataRoundTrip_largePayload();
  void test_handshake_peerAuth_trustedFingerprint();
  void test_handshake_peerAuth_untrustedFingerprint_fails();

private:
  // Arch must be initialised before any EventQueue is constructed because
  // EventQueue::EventQueue() registers signal handlers via ARCH->...
  // Log is required by CLOG / LOG_* macros used during construction.
  // Keep both as members so their lifetime spans every test method.
  std::unique_ptr<Arch> m_arch;
  std::unique_ptr<Log> m_log;
  QString m_certPath;
  QString m_trustedDir;
};
