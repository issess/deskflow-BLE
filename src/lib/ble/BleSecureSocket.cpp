/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BleSecureSocket.h"

#include "base/Event.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "net/Fingerprint.h"
#include "net/FingerprintDatabase.h"
#include "net/SecureUtils.h"
#include "net/SslLogger.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <QCryptographicHash>
#include <QFile>
#include <QString>
#include <algorithm>
#include <cstring>

namespace deskflow::ble {

namespace {
constexpr size_t kCipherChunkSize = 16 * 1024;

int ignoreCertVerify(X509_STORE_CTX *, void *)
{
  return 1;
}
} // namespace

BleSecureSocket::BleSecureSocket(IEventQueue *events, SecurityLevel level)
    : IDataSocket(events),
      m_events(events),
      m_transport(std::make_unique<BleSocket>(events)),
      m_level(level),
      m_isServer(false)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  wireTransportEventsLocked();
}

BleSecureSocket::BleSecureSocket(IEventQueue *events, std::unique_ptr<IDataSocket> transport, SecurityLevel level)
    : IDataSocket(events),
      m_events(events),
      m_transport(std::move(transport)),
      m_level(level),
      m_isServer(true)
{
  bool failed = false;
  std::string failReason;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    wireTransportEventsLocked();
    if (!initSslLocked(true)) {
      failed = true;
      failReason = "tls server init failed";
    }
  }
  if (failed)
    emitConnectFailed(failReason);
  // SSL_accept is driven once the transport delivers the ClientHello in
  // onTransportInputReady; nothing to do here yet.
}

BleSecureSocket::BleSecureSocket(
    IEventQueue *events, std::unique_ptr<IDataSocket> transport, SecurityLevel level, bool isServer)
    : IDataSocket(events),
      m_events(events),
      m_transport(std::move(transport)),
      m_level(level),
      m_isServer(isServer)
{
  bool failed = false;
  bool emitConn = false;
  std::string failReason;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    wireTransportEventsLocked();
    if (!initSslLocked(isServer)) {
      failed = true;
      failReason = "tls init failed";
    } else if (!isServer) {
      // Loopback assumes transport is already connected, so we kick the
      // client handshake immediately to put the ClientHello into outBio.
      driveHandshakeLocked(emitConn, failed, failReason);
    }
  }
  if (failed) {
    emitConnectFailed(failReason);
    return;
  }
  if (emitConn) {
    m_connectedEmitted = true;
    emitHandshakeDoneEvent();
  }
}

BleSecureSocket::~BleSecureSocket()
{
  unwireTransportEvents();
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    freeSslLocked();
  }
  if (m_events)
    m_events->removeHandlers(getEventTarget());
}

void BleSecureSocket::bind(const NetworkAddress &addr)
{
  if (m_transport)
    m_transport->bind(addr);
}

void BleSecureSocket::close()
{
  unwireTransportEvents();
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    freeSslLocked();
  }
  if (m_transport)
    m_transport->close();
  emitDisconnected();
}

void *BleSecureSocket::getEventTarget() const
{
  return const_cast<BleSecureSocket *>(this);
}

void BleSecureSocket::connect(const NetworkAddress &addr)
{
  if (!m_transport) {
    emitConnectFailed("transport missing");
    return;
  }
  m_transport->connect(addr);
}

bool BleSecureSocket::isFatal() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_fatal;
}

uint32_t BleSecureSocket::read(void *buffer, uint32_t n)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_plain.empty() || n == 0)
    return 0;
  uint32_t take = std::min<uint32_t>(n, static_cast<uint32_t>(m_plain.size()));
  if (buffer)
    std::memcpy(buffer, m_plain.data(), take);
  m_plain.erase(m_plain.begin(), m_plain.begin() + take);
  return take;
}

void BleSecureSocket::write(const void *buffer, uint32_t n)
{
  if (n == 0)
    return;
  bool fatalNow = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_handshakeDone || m_outputShutdown || m_fatal || !m_ssl) {
      LOG_DEBUG("BleSecureSocket::write before-ready/closed, dropping %u bytes", n);
      return;
    }
    int wrote = SSL_write(m_ssl, buffer, static_cast<int>(n));
    if (wrote <= 0) {
      const int err = SSL_get_error(m_ssl, wrote);
      if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
        char errBuf[256] = {};
        const unsigned long ossl = ERR_peek_last_error();
        ERR_error_string_n(ossl, errBuf, sizeof(errBuf));
        LOG_WARN("BleSecureSocket::write SSL_write err=%d ossl=0x%lx (%s) cumIn=%llu cumOut=%llu (%s)",
                 err, ossl, errBuf,
                 static_cast<unsigned long long>(m_cipherInBytes),
                 static_cast<unsigned long long>(m_cipherOutBytes),
                 m_isServer ? "server" : "client");
        m_fatal = true;
        fatalNow = true;
      }
      return;
    }
    m_plainOutBytes += static_cast<uint64_t>(wrote);
    LOG_DEBUG1("BleSecureSocket::write SSL_write n=%u wrote=%d cumPlainOut=%llu (%s)",
               n, wrote, static_cast<unsigned long long>(m_plainOutBytes),
               m_isServer ? "server" : "client");
    drainOutboundCipherLocked();
  }
  if (fatalNow)
    emitDisconnected();
}

void BleSecureSocket::flush()
{
  // SSL_write goes straight through SSL → BIO → transport on every call;
  // there's no separate buffer to flush.
}

void BleSecureSocket::shutdownInput()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inputShutdown = true;
  }
  if (m_transport)
    m_transport->shutdownInput();
  sendOurEvent(static_cast<int>(EventTypes::StreamInputShutdown));
}

void BleSecureSocket::shutdownOutput()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_outputShutdown = true;
  }
  if (m_transport)
    m_transport->shutdownOutput();
  sendOurEvent(static_cast<int>(EventTypes::StreamOutputShutdown));
}

bool BleSecureSocket::isReady() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return !m_plain.empty();
}

uint32_t BleSecureSocket::getSize() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<uint32_t>(m_plain.size());
}

// ---------------- Transport wiring ----------------

void BleSecureSocket::wireTransportEventsLocked()
{
  if (m_handlersWired || !m_transport)
    return;
  void *target = m_transport->getEventTarget();
  m_events->addHandler(EventTypes::DataSocketConnected, target, [this](const Event &) { onTransportConnected(); });
  m_events->addHandler(EventTypes::DataSocketConnectionFailed, target, [this](const Event &e) {
    auto *info = static_cast<IDataSocket::ConnectionFailedInfo *>(e.getData());
    onTransportConnectionFailed(info ? info->m_what : std::string("transport connection failed"));
  });
  m_events->addHandler(EventTypes::StreamInputReady, target, [this](const Event &) { onTransportInputReady(); });
  m_events->addHandler(EventTypes::SocketDisconnected, target, [this](const Event &) { onTransportDisconnected(); });
  m_handlersWired = true;
}

void BleSecureSocket::unwireTransportEvents()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_handlersWired || !m_transport || !m_events)
    return;
  m_events->removeHandlers(m_transport->getEventTarget());
  m_handlersWired = false;
}

// ---------------- Transport handlers ----------------

void BleSecureSocket::onTransportConnected()
{
  // Server SSL was set up in the constructor; only client side defers
  // initSsl until the BLE link is up.
  if (m_isServer)
    return;
  bool emitConn = false;
  bool failed = false;
  std::string failReason;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!initSslLocked(false)) {
      failed = true;
      failReason = "tls client init failed";
    } else {
      driveHandshakeLocked(emitConn, failed, failReason);
    }
  }
  if (failed) {
    emitConnectFailed(failReason);
    return;
  }
  if (emitConn) {
    m_connectedEmitted = true;
    emitHandshakeDoneEvent();
  }
}

void BleSecureSocket::emitHandshakeDoneEvent()
{
  // Match SecureSocket's wiring exactly:
  //   server post-SSL_accept  -> ClientListenerAccepted (so ClientListener
  //                              advances from "secure-pending" to client
  //                              proxy creation).
  //   client post-SSL_connect -> DataSocketConnected (Client.cpp's BLE
  //                              branch listens for this regardless of TLS
  //                              state — see Client.cpp:setupConnecting).
  if (m_isServer)
    sendOurEvent(static_cast<int>(EventTypes::ClientListenerAccepted));
  else
    sendOurEvent(static_cast<int>(EventTypes::DataSocketConnected));
}

void BleSecureSocket::onTransportConnectionFailed(const std::string &reason)
{
  emitConnectFailed(reason);
}

void BleSecureSocket::onTransportInputReady()
{
  bool emitConn = false;
  bool failed = false;
  std::string failReason;
  bool inputReadyEdge = false;
  bool inputShutdown = false;
  bool wasHandshakeDone = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    pullCiphertextFromTransportLocked();
    wasHandshakeDone = m_handshakeDone;
    if (!m_handshakeDone) {
      driveHandshakeLocked(emitConn, failed, failReason);
    }
    // Drain plaintext both during and after handshake — SSL_read on a
    // not-yet-finished SSL just returns WANT_READ which is harmless.
    if (!failed)
      pumpDecryptedLocked(inputReadyEdge, inputShutdown);
  }
  if (failed) {
    emitConnectFailed(failReason);
    return;
  }
  if (emitConn && !m_connectedEmitted) {
    m_connectedEmitted = true;
    emitHandshakeDoneEvent();
  }
  if (inputReadyEdge)
    sendOurEvent(static_cast<int>(EventTypes::StreamInputReady));
  if (inputShutdown)
    sendOurEvent(static_cast<int>(EventTypes::StreamInputShutdown));
  (void)wasHandshakeDone;
}

void BleSecureSocket::onTransportDisconnected()
{
  emitDisconnected();
}

// ---------------- SSL setup ----------------

bool BleSecureSocket::initSslLocked(bool server)
{
  if (m_ssl)
    return true;

  SSL_library_init();
  OpenSSL_add_all_algorithms();
  SSL_load_error_strings();
  SslLogger::logSecureLibInfo();

  const SSL_METHOD *method = server ? SSLv23_server_method() : SSLv23_client_method();
  m_ctx = SSL_CTX_new(method);
  if (!m_ctx) {
    SslLogger::logError();
    m_fatal = true;
    return false;
  }
  SSL_CTX_set_options(
      m_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 | SSL_OP_IGNORE_UNEXPECTED_EOF);

  if (m_level == SecurityLevel::PeerAuth) {
    SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    SSL_CTX_set_cert_verify_callback(m_ctx, ignoreCertVerify, nullptr);
  } else {
    SSL_CTX_set_cert_verify_callback(m_ctx, ignoreCertVerify, nullptr);
  }

  if (!loadCertificateLocked()) {
    LOG_ERR("BleSecureSocket: certificate load failed");
    m_fatal = true;
    return false;
  }

  m_ssl = SSL_new(m_ctx);
  if (!m_ssl) {
    SslLogger::logError();
    m_fatal = true;
    return false;
  }

  m_inBio = BIO_new(BIO_s_mem());
  m_outBio = BIO_new(BIO_s_mem());
  if (!m_inBio || !m_outBio) {
    LOG_ERR("BleSecureSocket: BIO_new failed");
    m_fatal = true;
    return false;
  }
  // SSL takes ownership of both BIOs.
  SSL_set_bio(m_ssl, m_inBio, m_outBio);

  if (server) {
    SSL_set_accept_state(m_ssl);
  } else {
    SSL_set_connect_state(m_ssl);
    const auto name = Settings::value(Settings::Core::ComputerName).toString().toStdString();
    if (!name.empty())
      SSL_set1_host(m_ssl, name.c_str());
  }

  return true;
}

bool BleSecureSocket::loadCertificateLocked()
{
  const QString filename = Settings::value(Settings::Security::Certificate).toString();
  if (filename.isEmpty()) {
    SslLogger::logError("tls certificate is not specified");
    return false;
  }
  if (!QFile::exists(filename)) {
    SslLogger::logError("tls certificate doesn't exist");
    return false;
  }
  const auto fName = filename.toStdString();
  if (SSL_CTX_use_certificate_file(m_ctx, fName.c_str(), SSL_FILETYPE_PEM) <= 0) {
    SslLogger::logError("could not use tls certificate");
    return false;
  }
  if (SSL_CTX_use_PrivateKey_file(m_ctx, fName.c_str(), SSL_FILETYPE_PEM) <= 0) {
    SslLogger::logError("could not use tls private key");
    return false;
  }
  if (!SSL_CTX_check_private_key(m_ctx)) {
    SslLogger::logError("could not verify tls private key");
    return false;
  }
  return true;
}

void BleSecureSocket::freeSslLocked()
{
  if (m_ssl) {
    SSL_set_quiet_shutdown(m_ssl, 1);
    SSL_shutdown(m_ssl);
    SSL_free(m_ssl);
    m_ssl = nullptr;
  }
  if (m_ctx) {
    SSL_CTX_free(m_ctx);
    m_ctx = nullptr;
  }
  // BIOs were owned by SSL via SSL_set_bio and freed with SSL_free.
  m_inBio = nullptr;
  m_outBio = nullptr;
}

// ---------------- SSL pipeline ----------------

void BleSecureSocket::driveHandshakeLocked(bool &outConnected, bool &outFailed, std::string &outFailReason)
{
  outConnected = false;
  outFailed = false;
  if (!m_ssl || m_fatal || m_handshakeDone)
    return;

  m_handshakeStarted = true;
  const int r = SSL_do_handshake(m_ssl);
  LOG_DEBUG1("BleSecureSocket: SSL_do_handshake r=%d sslErr=%d (%s)",
             r, SSL_get_error(m_ssl, r), m_isServer ? "server" : "client");
  drainOutboundCipherLocked();

  if (r == 1) {
    if (m_level == SecurityLevel::PeerAuth && !verifyPeerFingerprintLocked()) {
      m_fatal = true;
      outFailed = true;
      outFailReason = "peer fingerprint not trusted";
      return;
    }
    m_handshakeDone = true;
    outConnected = true;
    LOG_INFO("BleSecureSocket: TLS handshake complete (%s)", m_isServer ? "server" : "client");
    SslLogger::logSecureCipherInfo(m_ssl);
    SslLogger::logSecureConnectInfo(m_ssl);
    return;
  }

  const int err = SSL_get_error(m_ssl, r);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
    return;

  char errBuf[256];
  ERR_error_string_n(ERR_peek_last_error(), errBuf, sizeof(errBuf));
  LOG_ERR("BleSecureSocket: handshake failed err=%d (%s)", err, errBuf);
  m_fatal = true;
  outFailed = true;
  outFailReason = std::string("tls handshake failed: ") + errBuf;
}

void BleSecureSocket::drainOutboundCipherLocked()
{
  if (!m_outBio || !m_transport)
    return;
  uint8_t buf[kCipherChunkSize];
  uint64_t totalDrained = 0;
  int writes = 0;
  while (true) {
    const int got = BIO_read(m_outBio, buf, sizeof(buf));
    if (got <= 0)
      break;
    m_transport->write(buf, static_cast<uint32_t>(got));
    totalDrained += static_cast<uint64_t>(got);
    ++writes;
  }
  if (writes > 0) {
    m_cipherOutBytes += totalDrained;
    LOG_DEBUG1("BleSecureSocket: drainOutboundCipher writes=%d bytes=%llu cumOut=%llu (%s)",
               writes, static_cast<unsigned long long>(totalDrained),
               static_cast<unsigned long long>(m_cipherOutBytes), m_isServer ? "server" : "client");
  }
}

void BleSecureSocket::pullCiphertextFromTransportLocked()
{
  if (!m_inBio || !m_transport)
    return;
  uint8_t buf[kCipherChunkSize];
  uint64_t totalPulled = 0;
  int reads = 0;
  while (true) {
    const uint32_t got = m_transport->read(buf, sizeof(buf));
    if (got == 0)
      break;
    BIO_write(m_inBio, buf, static_cast<int>(got));
    totalPulled += got;
    ++reads;
  }
  if (reads > 0) {
    m_cipherInBytes += totalPulled;
    LOG_DEBUG1("BleSecureSocket: pullCiphertext reads=%d bytes=%llu cumIn=%llu (%s)",
               reads, static_cast<unsigned long long>(totalPulled),
               static_cast<unsigned long long>(m_cipherInBytes), m_isServer ? "server" : "client");
  }
}

void BleSecureSocket::pumpDecryptedLocked(bool &outInputReadyEdge, bool &outInputShutdown)
{
  outInputReadyEdge = false;
  outInputShutdown = false;
  if (!m_ssl || m_fatal)
    return;
  const bool wasEmpty = m_plain.empty();
  uint8_t buf[kCipherChunkSize];
  while (true) {
    const int n = SSL_read(m_ssl, buf, sizeof(buf));
    if (n > 0) {
      m_plain.insert(m_plain.end(), buf, buf + n);
      m_plainInBytes += static_cast<uint64_t>(n);
      LOG_DEBUG1("BleSecureSocket: SSL_read got=%d plainBuf=%zu cumPlainIn=%llu (%s)",
                 n, m_plain.size(), static_cast<unsigned long long>(m_plainInBytes),
                 m_isServer ? "server" : "client");
      continue;
    }
    const int err = SSL_get_error(m_ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
      break;
    if (err == SSL_ERROR_ZERO_RETURN) {
      m_inputShutdown = true;
      outInputShutdown = true;
      LOG_DEBUG("BleSecureSocket: SSL_read clean shutdown (%s)", m_isServer ? "server" : "client");
      break;
    }
    char errBuf[256] = {};
    const unsigned long ossl = ERR_peek_last_error();
    ERR_error_string_n(ossl, errBuf, sizeof(errBuf));
    LOG_WARN("BleSecureSocket: SSL_read err=%d ossl=0x%lx (%s) cumIn=%llu cumOut=%llu plainIn=%llu plainOut=%llu (%s)",
             err, ossl, errBuf,
             static_cast<unsigned long long>(m_cipherInBytes),
             static_cast<unsigned long long>(m_cipherOutBytes),
             static_cast<unsigned long long>(m_plainInBytes),
             static_cast<unsigned long long>(m_plainOutBytes),
             m_isServer ? "server" : "client");
    // Drain the entire OpenSSL error queue so we see the full chain (e.g.
    // bad record MAC + decryption failed) instead of just the last entry.
    while (true) {
      const unsigned long e = ERR_get_error();
      if (e == 0)
        break;
      ERR_error_string_n(e, errBuf, sizeof(errBuf));
      LOG_WARN("BleSecureSocket: openssl err 0x%lx (%s)", e, errBuf);
    }
    m_fatal = true;
    break;
  }
  if (wasEmpty && !m_plain.empty())
    outInputReadyEdge = true;
}

bool BleSecureSocket::verifyPeerFingerprintLocked()
{
  X509 *peerCert = SSL_get_peer_certificate(m_ssl);
  const Fingerprint fp = deskflow::sslCertFingerprint(peerCert, QCryptographicHash::Sha256);
  if (peerCert)
    X509_free(peerCert);
  if (!fp.isValid()) {
    LOG_ERR("BleSecureSocket: peer cert has no valid fingerprint");
    return false;
  }
  const QString dbPath = m_isServer ? Settings::tlsTrustedClientsDb() : Settings::tlsTrustedServersDb();
  FingerprintDatabase db;
  db.read(dbPath);
  if (!db.isTrusted(fp)) {
    LOG_ERR("BleSecureSocket: peer fingerprint not in trusted DB (%s)", qPrintable(dbPath));
    return false;
  }
  LOG_INFO("BleSecureSocket: peer fingerprint trusted");
  return true;
}

// ---------------- Event helpers ----------------

void BleSecureSocket::sendOurEvent(int type)
{
  m_events->addEvent(Event(static_cast<EventTypes>(type), getEventTarget()));
}

void BleSecureSocket::emitConnectFailed(const std::string &reason)
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fatal = true;
  }
  auto *info = new IDataSocket::ConnectionFailedInfo(reason.c_str());
  m_events->addEvent(
      Event(EventTypes::DataSocketConnectionFailed, getEventTarget(), info, Event::EventFlags::DontFreeData));
}

void BleSecureSocket::emitDisconnected()
{
  bool first = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_disconnectedEmitted) {
      m_disconnectedEmitted = true;
      first = true;
    }
  }
  if (first)
    sendOurEvent(static_cast<int>(EventTypes::SocketDisconnected));
}

} // namespace deskflow::ble
