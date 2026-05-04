/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "BleSecureSocketTests.h"

#include "base/Event.h"
#include "base/EventQueue.h"
#include "base/EventTypes.h"
#include "ble/BleSecureSocket.h"
#include "common/Settings.h"
#include "net/IDataSocket.h"
#include "net/SecureUtils.h"
#include "net/SecurityLevel.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTextStream>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using deskflow::ble::BleSecureSocket;
using deskflow::EventTypes;

// Stub: production builds resolve this through the `app` static library
// (deskflow/ipc/CoreIpc.cpp). The unit test exe doesn't run an IPC server,
// so we just need the symbol to resolve — calls become no-ops.
void ipcSendToClient(const QString &, const QString &)
{
}

namespace {

// In-memory IDataSocket used to splice two BleSecureSocket peers together
// for handshake / round-trip tests. write() on this socket pushes bytes
// into the paired peer's read buffer and queues a StreamInputReady event
// at the peer's event target so the peer's handler runs through the
// EventQueue dispatch path.
class LoopbackDataSocket : public IDataSocket
{
public:
  explicit LoopbackDataSocket(IEventQueue *events) : IDataSocket(events), m_events(events)
  {
  }

  void pair(LoopbackDataSocket *peer)
  {
    m_peer = peer;
  }

  // Inject bytes received "from the peer". Called by the paired peer's
  // write(); fires StreamInputReady so the receiving BleSecureSocket
  // pumps them through SSL.
  void deliverInbound(const uint8_t *data, uint32_t n)
  {
    {
      std::lock_guard<std::mutex> lock(m_mu);
      m_inbound.insert(m_inbound.end(), data, data + n);
    }
    m_events->addEvent(Event(EventTypes::StreamInputReady, getEventTarget()));
  }

  // ISocket
  void bind(const NetworkAddress &) override
  {
  }
  void close() override
  {
    bool first = false;
    {
      std::lock_guard<std::mutex> lock(m_mu);
      first = !m_closed;
      m_closed = true;
    }
    if (first)
      m_events->addEvent(Event(EventTypes::SocketDisconnected, getEventTarget()));
  }
  void *getEventTarget() const override
  {
    return const_cast<LoopbackDataSocket *>(this);
  }

  // IDataSocket
  void connect(const NetworkAddress &) override
  {
  }
  bool isFatal() const override
  {
    return false;
  }

  // IStream
  uint32_t read(void *buffer, uint32_t n) override
  {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_inbound.empty() || n == 0)
      return 0;
    uint32_t take = std::min<uint32_t>(n, static_cast<uint32_t>(m_inbound.size()));
    if (buffer)
      std::memcpy(buffer, m_inbound.data(), take);
    m_inbound.erase(m_inbound.begin(), m_inbound.begin() + take);
    return take;
  }
  void write(const void *buffer, uint32_t n) override
  {
    if (!m_peer || n == 0 || m_closed)
      return;
    m_peer->deliverInbound(static_cast<const uint8_t *>(buffer), n);
  }
  void flush() override
  {
  }
  void shutdownInput() override
  {
  }
  void shutdownOutput() override
  {
  }
  bool isReady() const override
  {
    std::lock_guard<std::mutex> lock(m_mu);
    return !m_inbound.empty();
  }
  uint32_t getSize() const override
  {
    std::lock_guard<std::mutex> lock(m_mu);
    return static_cast<uint32_t>(m_inbound.size());
  }

private:
  IEventQueue *m_events;
  LoopbackDataSocket *m_peer = nullptr;
  mutable std::mutex m_mu;
  std::vector<uint8_t> m_inbound;
  bool m_closed = false;
};

// RAII for an EventQueue running its dispatch loop on a background thread.
class EventLoopHarness
{
public:
  EventLoopHarness() : m_thread([this] { m_events.loop(); })
  {
    // EventQueue::loop() signals readiness via condvar — wait briefly so
    // tests can register handlers and post events knowing they will be
    // dispatched. A 50 ms sleep is generous for the buffer init step.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ~EventLoopHarness()
  {
    m_events.addEvent(Event(EventTypes::Quit));
    if (m_thread.joinable())
      m_thread.join();
  }
  IEventQueue *events()
  {
    return &m_events;
  }

private:
  EventQueue m_events;
  std::thread m_thread;
};

// Poll until cond() returns true or timeoutMs elapses. Yields between
// checks so the EventQueue thread can make progress without us hot-looping.
template <typename Cond> bool waitFor(Cond cond, int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return cond();
}

QString getCertPath()
{
  const auto dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  return QStringLiteral("%1/deskflow-ble-secure-test-cert.pem").arg(dir);
}

QString getTrustedDir()
{
  const auto dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  return QStringLiteral("%1/deskflow-ble-secure-test-trusted").arg(dir);
}

QByteArray sha256OfCertFile(const QString &path)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  const auto pem = f.readAll();
  BIO *bio = BIO_new_mem_buf(pem.constData(), pem.size());
  if (!bio)
    return {};
  X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!cert)
    return {};
  unsigned char buf[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  X509_digest(cert, EVP_sha256(), buf, &len);
  X509_free(cert);
  return QByteArray(reinterpret_cast<const char *>(buf), int(len));
}

void writeTrustedDb(const QString &dbPath, const QByteArray &sha256)
{
  QFileInfo info(dbPath);
  QDir().mkpath(info.absolutePath());
  QFile f(dbPath);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return;
  QTextStream out(&f);
  out << "v2:sha256:" << QString::fromLatin1(sha256.toHex().toLower()) << "\n";
}

} // namespace

void BleSecureSocketTests::initTestCase()
{
  m_arch = std::make_unique<Arch>();
  m_arch->init();
  m_log = std::make_unique<Log>();

  m_certPath = getCertPath();
  m_trustedDir = getTrustedDir();
  // A 2048-bit RSA self-signed cert is plenty for handshake tests and
  // roughly the smallest the codebase generates elsewhere.
  if (!QFile::exists(m_certPath)) {
    deskflow::generatePemSelfSignedCert(m_certPath, 2048);
  }
  QVERIFY(QFile::exists(m_certPath));

  // Point Settings at our test cert so initSslLocked picks it up.
  Settings::setSettingsFile(QDir::temp().filePath("deskflow-ble-secure-test.ini"));
  Settings::setStateFile(QDir::temp().filePath("deskflow-ble-secure-test-state.ini"));
  Settings::setValue(Settings::Security::Certificate, m_certPath);
  Settings::save();
}

void BleSecureSocketTests::cleanupTestCase()
{
  // Leave the cert alone so reruns are fast — Settings file is small and
  // re-initialised per run.
}

void BleSecureSocketTests::test_construct_serverSide_validCert()
{
  EventLoopHarness h;
  auto *raw = new LoopbackDataSocket(h.events());
  BleSecureSocket s(h.events(), std::unique_ptr<IDataSocket>(raw), SecurityLevel::Encrypted);
  QVERIFY(s.getEventTarget() != nullptr);
  QVERIFY(!s.isFatal());
  QCOMPARE(s.getSize(), uint32_t(0));
  QVERIFY(!s.isReady());
}

void BleSecureSocketTests::test_construct_serverSide_missingCert_emitsFail()
{
  EventLoopHarness h;
  // Point Settings at a path that doesn't exist so loadCertificateLocked
  // fails, which should mark the socket fatal and emit a connection-failed
  // event at the wrapper's target.
  const auto good = m_certPath;
  Settings::setValue(Settings::Security::Certificate, QStringLiteral("C:/__definitely_does_not_exist__.pem"));

  std::atomic_bool failed{false};
  void *target = nullptr;

  // We need the target before construction to subscribe — but the target
  // is the BleSecureSocket pointer itself. Workaround: subscribe inline
  // after construction; the failure is queued via addEvent so dispatch is
  // deferred and we'll catch it.
  auto *raw = new LoopbackDataSocket(h.events());
  BleSecureSocket s(h.events(), std::unique_ptr<IDataSocket>(raw), SecurityLevel::Encrypted);
  target = s.getEventTarget();
  h.events()->addHandler(EventTypes::DataSocketConnectionFailed, target,
                         [&failed](const Event &) { failed.store(true); });

  QVERIFY(waitFor([&] { return failed.load(); }, 1000));
  QVERIFY(s.isFatal());

  // Restore for subsequent tests.
  Settings::setValue(Settings::Security::Certificate, good);
}

void BleSecureSocketTests::test_close_idempotent()
{
  EventLoopHarness h;
  auto *raw = new LoopbackDataSocket(h.events());
  BleSecureSocket s(h.events(), std::unique_ptr<IDataSocket>(raw), SecurityLevel::Encrypted);

  std::atomic<int> disconnectedCount{0};
  h.events()->addHandler(EventTypes::SocketDisconnected, s.getEventTarget(),
                         [&](const Event &) { disconnectedCount.fetch_add(1); });

  s.close();
  s.close();
  s.close();
  QVERIFY(waitFor([&] { return disconnectedCount.load() >= 1; }, 500));
  // Must only fire once even across repeat close()s.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  QCOMPARE(disconnectedCount.load(), 1);
}

void BleSecureSocketTests::test_read_beforeHandshake_returnsZero()
{
  EventLoopHarness h;
  auto *raw = new LoopbackDataSocket(h.events());
  BleSecureSocket s(h.events(), std::unique_ptr<IDataSocket>(raw), SecurityLevel::Encrypted);
  char buf[64] = {};
  QCOMPARE(s.read(buf, sizeof(buf)), uint32_t(0));
}

void BleSecureSocketTests::test_isReady_beforeHandshake_false()
{
  EventLoopHarness h;
  auto *raw = new LoopbackDataSocket(h.events());
  BleSecureSocket s(h.events(), std::unique_ptr<IDataSocket>(raw), SecurityLevel::Encrypted);
  QVERIFY(!s.isReady());
}

void BleSecureSocketTests::test_getSize_beforeHandshake_zero()
{
  EventLoopHarness h;
  auto *raw = new LoopbackDataSocket(h.events());
  BleSecureSocket s(h.events(), std::unique_ptr<IDataSocket>(raw), SecurityLevel::Encrypted);
  QCOMPARE(s.getSize(), uint32_t(0));
}

void BleSecureSocketTests::test_getEventTarget_stable()
{
  EventLoopHarness h;
  auto *raw = new LoopbackDataSocket(h.events());
  BleSecureSocket s(h.events(), std::unique_ptr<IDataSocket>(raw), SecurityLevel::Encrypted);
  void *t1 = s.getEventTarget();
  void *t2 = s.getEventTarget();
  QCOMPARE(t1, t2);
}

void BleSecureSocketTests::test_handshake_encrypted_completes()
{
  EventLoopHarness h;
  auto *serverT = new LoopbackDataSocket(h.events());
  auto *clientT = new LoopbackDataSocket(h.events());
  serverT->pair(clientT);
  clientT->pair(serverT);

  BleSecureSocket server(h.events(), std::unique_ptr<IDataSocket>(serverT), SecurityLevel::Encrypted, /*isServer=*/true);
  BleSecureSocket client(h.events(), std::unique_ptr<IDataSocket>(clientT), SecurityLevel::Encrypted, /*isServer=*/false);

  std::atomic_bool serverDone{false};
  std::atomic_bool clientDone{false};
  // Server-side post-handshake event is ClientListenerAccepted (mirrors
  // SecureSocket); client-side is DataSocketConnected.
  h.events()->addHandler(EventTypes::ClientListenerAccepted, server.getEventTarget(),
                         [&](const Event &) { serverDone.store(true); });
  h.events()->addHandler(EventTypes::DataSocketConnected, client.getEventTarget(),
                         [&](const Event &) { clientDone.store(true); });

  QVERIFY2(waitFor([&] { return serverDone.load() && clientDone.load(); }, 5000),
           "TLS handshake did not complete within 5 s");
  QVERIFY(!server.isFatal());
  QVERIFY(!client.isFatal());
}

void BleSecureSocketTests::test_dataRoundTrip_encrypted()
{
  EventLoopHarness h;
  auto *serverT = new LoopbackDataSocket(h.events());
  auto *clientT = new LoopbackDataSocket(h.events());
  serverT->pair(clientT);
  clientT->pair(serverT);

  BleSecureSocket server(h.events(), std::unique_ptr<IDataSocket>(serverT), SecurityLevel::Encrypted, true);
  BleSecureSocket client(h.events(), std::unique_ptr<IDataSocket>(clientT), SecurityLevel::Encrypted, false);

  std::atomic_bool serverDone{false}, clientDone{false};
  h.events()->addHandler(EventTypes::ClientListenerAccepted, server.getEventTarget(),
                         [&](const Event &) { serverDone.store(true); });
  h.events()->addHandler(EventTypes::DataSocketConnected, client.getEventTarget(),
                         [&](const Event &) { clientDone.store(true); });
  QVERIFY(waitFor([&] { return serverDone.load() && clientDone.load(); }, 5000));

  // Client → server.
  const QByteArray msgC2S = "hello from client";
  client.write(msgC2S.constData(), uint32_t(msgC2S.size()));
  QVERIFY(waitFor([&] { return server.getSize() >= uint32_t(msgC2S.size()); }, 2000));
  QByteArray gotS(msgC2S.size(), '\0');
  QCOMPARE(server.read(gotS.data(), uint32_t(gotS.size())), uint32_t(msgC2S.size()));
  QCOMPARE(gotS, msgC2S);

  // Server → client.
  const QByteArray msgS2C = "and back from server";
  server.write(msgS2C.constData(), uint32_t(msgS2C.size()));
  QVERIFY(waitFor([&] { return client.getSize() >= uint32_t(msgS2C.size()); }, 2000));
  QByteArray gotC(msgS2C.size(), '\0');
  QCOMPARE(client.read(gotC.data(), uint32_t(gotC.size())), uint32_t(msgS2C.size()));
  QCOMPARE(gotC, msgS2C);
}

void BleSecureSocketTests::test_dataRoundTrip_largePayload()
{
  EventLoopHarness h;
  auto *serverT = new LoopbackDataSocket(h.events());
  auto *clientT = new LoopbackDataSocket(h.events());
  serverT->pair(clientT);
  clientT->pair(serverT);

  BleSecureSocket server(h.events(), std::unique_ptr<IDataSocket>(serverT), SecurityLevel::Encrypted, true);
  BleSecureSocket client(h.events(), std::unique_ptr<IDataSocket>(clientT), SecurityLevel::Encrypted, false);

  std::atomic_bool serverDone{false}, clientDone{false};
  h.events()->addHandler(EventTypes::ClientListenerAccepted, server.getEventTarget(),
                         [&](const Event &) { serverDone.store(true); });
  h.events()->addHandler(EventTypes::DataSocketConnected, client.getEventTarget(),
                         [&](const Event &) { clientDone.store(true); });
  QVERIFY(waitFor([&] { return serverDone.load() && clientDone.load(); }, 5000));

  // 64 KiB exercises spanning multiple TLS records (default ~16 KiB record
  // boundary). Each direction sends a fresh pattern.
  QByteArray big(64 * 1024, Qt::Uninitialized);
  for (int i = 0; i < big.size(); ++i)
    big[i] = char(i & 0xff);

  client.write(big.constData(), uint32_t(big.size()));
  QVERIFY2(waitFor([&] { return server.getSize() >= uint32_t(big.size()); }, 5000),
           "server did not receive full 64 KiB within 5 s");
  QByteArray got(big.size(), '\0');
  QCOMPARE(server.read(got.data(), uint32_t(got.size())), uint32_t(big.size()));
  QCOMPARE(got, big);
}

void BleSecureSocketTests::test_handshake_peerAuth_trustedFingerprint()
{
  EventLoopHarness h;

  // Both ends use the same self-signed cert (peer == self == trusted).
  // Pre-populate the trusted-clients DB the server checks against.
  QDir().mkpath(m_trustedDir);
  Settings::setValue(Settings::Security::Certificate, m_certPath);

  // Point fingerprint DBs (both directions, so server and client both
  // accept the peer cert).
  const auto fp = sha256OfCertFile(m_certPath);
  QVERIFY(!fp.isEmpty());
  // The runtime resolves these via Settings::tlsTrustedClientsDb() /
  // Settings::tlsTrustedServersDb(); both end up in
  // %APPDATA%/Deskflow/tls/trusted-* by default. Write to both.
  const auto base = QDir::temp().filePath("deskflow-ble-secure-test-tls");
  QDir().mkpath(base);
  // The test runs against the live Settings::tlsTrustedClientsDb()/...
  // which we cannot redirect cleanly here. Instead, query the actual paths
  // and write into them. Both loopback peers use the same cert, so both
  // DBs need the same fingerprint.
  writeTrustedDb(Settings::tlsTrustedClientsDb(), fp);
  writeTrustedDb(Settings::tlsTrustedServersDb(), fp);

  auto *serverT = new LoopbackDataSocket(h.events());
  auto *clientT = new LoopbackDataSocket(h.events());
  serverT->pair(clientT);
  clientT->pair(serverT);

  BleSecureSocket server(h.events(), std::unique_ptr<IDataSocket>(serverT), SecurityLevel::PeerAuth, true);
  BleSecureSocket client(h.events(), std::unique_ptr<IDataSocket>(clientT), SecurityLevel::PeerAuth, false);

  std::atomic_bool serverDone{false}, clientDone{false};
  h.events()->addHandler(EventTypes::ClientListenerAccepted, server.getEventTarget(),
                         [&](const Event &) { serverDone.store(true); });
  h.events()->addHandler(EventTypes::DataSocketConnected, client.getEventTarget(),
                         [&](const Event &) { clientDone.store(true); });

  QVERIFY2(waitFor([&] { return serverDone.load() && clientDone.load(); }, 5000),
           "PeerAuth handshake with trusted fingerprint did not complete");
  QVERIFY(!server.isFatal());
  QVERIFY(!client.isFatal());
}

void BleSecureSocketTests::test_handshake_peerAuth_untrustedFingerprint_fails()
{
  EventLoopHarness h;

  // Wipe trusted DBs so the peer cert won't be recognised.
  QFile::remove(Settings::tlsTrustedClientsDb());
  QFile::remove(Settings::tlsTrustedServersDb());
  Settings::setValue(Settings::Security::Certificate, m_certPath);

  auto *serverT = new LoopbackDataSocket(h.events());
  auto *clientT = new LoopbackDataSocket(h.events());
  serverT->pair(clientT);
  clientT->pair(serverT);

  BleSecureSocket server(h.events(), std::unique_ptr<IDataSocket>(serverT), SecurityLevel::PeerAuth, true);
  BleSecureSocket client(h.events(), std::unique_ptr<IDataSocket>(clientT), SecurityLevel::PeerAuth, false);

  std::atomic_bool clientFailed{false};
  std::atomic_bool serverFailed{false};
  h.events()->addHandler(EventTypes::DataSocketConnectionFailed, client.getEventTarget(),
                         [&](const Event &) { clientFailed.store(true); });
  h.events()->addHandler(EventTypes::DataSocketConnectionFailed, server.getEventTarget(),
                         [&](const Event &) { serverFailed.store(true); });

  // At least one end must reject the peer fingerprint within the timeout.
  QVERIFY2(waitFor([&] { return clientFailed.load() || serverFailed.load(); }, 5000),
           "PeerAuth handshake should fail without a trusted fingerprint");
}

QTEST_MAIN(BleSecureSocketTests)
