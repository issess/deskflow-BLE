/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ble/BleSocket.h"
#include "net/IDataSocket.h"
#include "net/SecurityLevel.h"

#include <memory>
#include <mutex>
#include <vector>

extern "C" {
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct bio_st BIO;
}

namespace deskflow::ble {

// TLS-over-BLE wrapper. Owns a BleSocket as its transport and runs OpenSSL
// on top of two memory BIOs: incoming BLE chunks are fed into m_inBio so
// SSL_read sees them, SSL writes go into m_outBio and we forward them out
// via the transport. Mirrors the TCP SecureSocket policy:
//   - SecurityLevel::Encrypted  -> handshake + cipher, no peer verification
//   - SecurityLevel::PeerAuth   -> additionally checks the peer fingerprint
//                                  against the same trusted-peer DB used by
//                                  the TCP path.
class BleSecureSocket : public IDataSocket
{
public:
  // Client-side. The BleSocket is created internally; connect() drives the
  // BLE handshake first, then the TLS handshake on top.
  BleSecureSocket(IEventQueue *events, SecurityLevel level);

  // Server-side. Adopt an already-connected transport (returned by
  // BleListenSocket::accept()). SSL_accept is set up immediately and
  // progresses on the first incoming chunk (the ClientHello). Accepts
  // any IDataSocket so unit tests can substitute an in-memory loopback
  // transport in place of a real BleSocket.
  BleSecureSocket(IEventQueue *events, std::unique_ptr<IDataSocket> transport, SecurityLevel level);

  // Test-flexible constructor that does not wait for the transport's
  // DataSocketConnected — assumes the transport is already connected when
  // passed in — and drives SSL_connect or SSL_accept immediately based on
  // the isServer flag. Used by unit tests with an in-memory loopback
  // transport; production code goes through one of the two-/three-arg
  // constructors above.
  BleSecureSocket(
      IEventQueue *events, std::unique_ptr<IDataSocket> transport, SecurityLevel level, bool isServer);

  ~BleSecureSocket() override;

  // ISocket
  void bind(const NetworkAddress &addr) override;
  void close() override;
  void *getEventTarget() const override;

  // IDataSocket
  void connect(const NetworkAddress &addr) override;
  bool isFatal() const override;

  // IStream — plaintext for the upper layer.
  uint32_t read(void *buffer, uint32_t n) override;
  void write(const void *buffer, uint32_t n) override;
  void flush() override;
  void shutdownInput() override;
  void shutdownOutput() override;
  bool isReady() const override;
  uint32_t getSize() const override;

private:
  void wireTransportEventsLocked();
  void unwireTransportEvents();

  // Transport-event handlers (run on the event-queue thread).
  void onTransportConnected();
  void onTransportConnectionFailed(const std::string &reason);
  void onTransportInputReady();
  void onTransportDisconnected();

  // SSL pipeline — all expect m_mutex held.
  bool initSslLocked(bool server);
  void freeSslLocked();
  bool loadCertificateLocked();
  void driveHandshakeLocked(bool &outConnected, bool &outFailed, std::string &outFailReason);
  void drainOutboundCipherLocked();
  void pullCiphertextFromTransportLocked();
  void pumpDecryptedLocked(bool &outInputReadyEdge, bool &outInputShutdown);
  bool verifyPeerFingerprintLocked();

  // Event helpers — must NOT be called while holding m_mutex (they may
  // deliver to handlers that re-enter us via close()/read()/write()).
  void sendOurEvent(int type);
  void emitHandshakeDoneEvent();
  void emitConnectFailed(const std::string &reason);
  void emitDisconnected();

  IEventQueue *m_events;
  std::unique_ptr<IDataSocket> m_transport;
  SecurityLevel m_level;
  bool m_isServer;

  mutable std::mutex m_mutex;
  SSL_CTX *m_ctx = nullptr;
  SSL *m_ssl = nullptr;
  BIO *m_inBio = nullptr;  // peer ciphertext lands here (BIO_write)
  BIO *m_outBio = nullptr; // SSL writes ciphertext here (BIO_read by us)

  bool m_handshakeStarted = false;
  bool m_handshakeDone = false;
  bool m_connectedEmitted = false;
  bool m_disconnectedEmitted = false;
  bool m_fatal = false;
  bool m_inputShutdown = false;
  bool m_outputShutdown = false;
  bool m_handlersWired = false;

  // Plaintext buffer for upper-layer reads.
  std::vector<uint8_t> m_plain;

  // Diagnostic byte counters (DEBUG logging only).
  uint64_t m_cipherInBytes = 0;  // ciphertext pulled from transport into BIO
  uint64_t m_cipherOutBytes = 0; // ciphertext drained from BIO to transport
  uint64_t m_plainInBytes = 0;   // plaintext successfully decrypted
  uint64_t m_plainOutBytes = 0;  // plaintext submitted to SSL_write
};

} // namespace deskflow::ble
