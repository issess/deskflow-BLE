/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/BleListenSocket.h"

#include "base/Event.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "ble/BlePairingBroker.h"
#include "ble/BleSocket.h"
#include "ble/BleTransport.h"
#include "ble/IBlePeripheralBackend.h"
#include "ble/qt/QtBlePeripheralBackend.h"
#include "common/Settings.h"

#ifdef Q_OS_WIN
#include "ble/win/WinRtBlePeripheralBackend.h"
#endif

#include <QCoreApplication>
#include <QTextStream>
#include <QThread>
#include <QTimer>

namespace deskflow::ble {

namespace {

IBlePeripheralBackend *createBackend(QObject *parent)
{
#ifdef Q_OS_WIN
  const auto backend = Settings::value(Settings::Core::BleBackend).toString().toLower();
  if (backend == QStringLiteral("qt")) {
    LOG_NOTE("BLE peripheral backend selected: QtBluetooth");
    return new QtBlePeripheralBackend(parent);
  }
  LOG_NOTE("BLE peripheral backend selected: WinRT");
  // Windows: use direct C++/WinRT path. Qt's own peripheral backend on
  // Windows fails on a wide range of consumer BT adapters.
  return new WinRtBlePeripheralBackend(parent);
#else
  LOG_NOTE("BLE peripheral backend selected: QtBluetooth");
  return new QtBlePeripheralBackend(parent);
#endif
}

} // namespace

//
// BlePeripheralContext
//

BlePeripheralContext::BlePeripheralContext(BleListenSocket *owner) : QObject(nullptr), m_owner(owner)
{
  m_timeout = new QTimer(this);
  m_timeout->setSingleShot(true);
  m_timeout->setInterval(kPairingTimeoutSeconds * 1000);
  QObject::connect(m_timeout, &QTimer::timeout, this, &BlePeripheralContext::onTimeout);
}

BlePeripheralContext::~BlePeripheralContext()
{
  tearDown();
}

bool BlePeripheralContext::start()
{
  LOG_NOTE("BlePeripheralContext::start");
  if (m_backend) {
    LOG_WARN("BLE peripheral already started");
    return false;
  }

  publishCode();

  m_backend = createBackend(this);
  QObject::connect(m_backend, &IBlePeripheralBackend::started, this, &BlePeripheralContext::onBackendStarted);
  QObject::connect(m_backend, &IBlePeripheralBackend::startFailed, this,
                   &BlePeripheralContext::onBackendStartFailed);
  QObject::connect(m_backend, &IBlePeripheralBackend::pairingAuthWritten, this,
                   &BlePeripheralContext::onPairingAuthWritten);
  QObject::connect(m_backend, &IBlePeripheralBackend::upstreamWritten, this,
                   &BlePeripheralContext::onUpstreamWritten);
  QObject::connect(m_backend, &IBlePeripheralBackend::centralConnected, this,
                   &BlePeripheralContext::onCentralConnected);
  QObject::connect(m_backend, &IBlePeripheralBackend::centralDisconnected, this,
                   &BlePeripheralContext::onCentralDisconnected);
  QObject::connect(m_backend, &IBlePeripheralBackend::mtuChanged, this, &BlePeripheralContext::onMtuChanged);

  const QByteArray mfgPayload = buildManufacturerData();
  const bool ok = m_backend->start(QStringLiteral("Deskflow"), mfgPayload);
  if (!ok) {
    LOG_ERR("BLE peripheral backend start returned false synchronously");
    return false;
  }
  m_timeout->start();
  LOG_NOTE("BLE pairing started; code=%s", m_currentCode.toUtf8().constData());
  return true;
}

void BlePeripheralContext::stop()
{
  tearDown();
}

void BlePeripheralContext::publishCode()
{
  m_currentCode = m_code.generate();
  BlePairingBroker::instance().setActiveCode(m_currentCode);
  QTextStream(stdout) << "BLE_PAIRING:CODE=" << m_currentCode << Qt::endl;
  m_attempts = 0;
  m_paired = false;
}

void BlePeripheralContext::resetCode()
{
  m_code.clear();
  m_currentCode.clear();
  BlePairingBroker::instance().clearActiveCode();
  QTextStream(stdout) << "BLE_PAIRING:CODE=" << Qt::endl;
}

void BlePeripheralContext::tearDown()
{
  if (m_timeout)
    m_timeout->stop();
  if (m_backend) {
    m_backend->stop();
    m_backend->deleteLater();
    m_backend = nullptr;
  }
  resetCode();
}

QByteArray BlePeripheralContext::buildManufacturerData() const
{
  QByteArray data;
  data.append(static_cast<char>((kAdvertMagic >> 8) & 0xFF));
  data.append(static_cast<char>(kAdvertMagic & 0xFF));
  data.append(BlePairingCode::hashPrefix(m_currentCode));
  return data;
}

void BlePeripheralContext::onBackendStarted()
{
  LOG_NOTE("BLE backend started (advertising)");
}

void BlePeripheralContext::onBackendStartFailed(const QString &reason)
{
  LOG_ERR("BLE backend start failed: %s", reason.toUtf8().constData());
  QTextStream(stdout) << "BLE_PAIRING:RESULT=rejected:" << reason << Qt::endl;
  BlePairingBroker::instance().reportResult(false, reason);
  tearDown();
}

void BlePeripheralContext::onPairingAuthWritten(const QByteArray &value)
{
  if (m_paired)
    return;
  const QString candidate = QString::fromUtf8(value).trimmed();
  if (!m_code.verify(candidate)) {
    ++m_attempts;
    LOG_WARN("BLE pairing: bad code (attempt %d/%d)", m_attempts, kPairingMaxAttempts);
    if (m_backend)
      m_backend->sendPairingStatus(static_cast<quint8>(PairingStatus::Rejected));
    if (m_attempts >= kPairingMaxAttempts) {
      BlePairingBroker::instance().reportResult(false, QStringLiteral("too many attempts"));
      QTextStream(stdout) << "BLE_PAIRING:RESULT=rejected:too many attempts" << Qt::endl;
      tearDown();
    }
    return;
  }

  // Success.
  m_paired = true;
  m_timeout->stop();
  m_code.clear();
  m_currentCode.clear();
  BlePairingBroker::instance().clearActiveCode();
  if (m_backend)
    m_backend->sendPairingStatus(static_cast<quint8>(PairingStatus::Accepted));
  QTextStream(stdout) << "BLE_PAIRING:RESULT=accepted:" << Qt::endl;

  // Create and hand over the BleSocket.
  auto socket = std::make_unique<BleSocket>(m_owner->events());
  socket->adoptPeripheralBackend(m_backend, m_negotiatedMtu > 0 ? m_negotiatedMtu : 247);
  m_acceptedSocket = socket.get();
  m_owner->pushAcceptedSocket(std::move(socket));
  m_owner->events()->addEvent(Event(EventTypes::ListenSocketConnecting, m_owner->getEventTarget()));
  BlePairingBroker::instance().reportResult(true, QString());
}

void BlePeripheralContext::onUpstreamWritten(const QByteArray &value)
{
  LOG_DEBUG("BlePeripheralContext::onUpstreamWritten size=%d acceptedSocket=%p",
            value.size(), (void *)m_acceptedSocket);
  if (m_acceptedSocket)
    m_acceptedSocket->deliverInbound(value);
}

void BlePeripheralContext::onCentralConnected()
{
  LOG_NOTE("BLE central subscribed (peripheral side notified)");
}

void BlePeripheralContext::onCentralDisconnected()
{
  LOG_NOTE("BLE central unsubscribed acceptedSocket=%p", (void *)m_acceptedSocket);
  if (m_acceptedSocket) {
    m_acceptedSocket->notifyDisconnected();
    m_acceptedSocket = nullptr;
  }
}

void BlePeripheralContext::onMtuChanged(int mtu)
{
  m_negotiatedMtu = mtu;
  if (m_acceptedSocket)
    m_acceptedSocket->updateMtu(mtu);
}

void BlePeripheralContext::onTimeout()
{
  if (m_paired)
    return;
  LOG_NOTE("BLE pairing timed out after %d seconds", kPairingTimeoutSeconds);
  BlePairingBroker::instance().reportResult(false, QStringLiteral("timeout"));
  QTextStream(stdout) << "BLE_PAIRING:RESULT=rejected:timeout" << Qt::endl;
  tearDown();
}

//
// BleListenSocket
//

BleListenSocket::BleListenSocket(IEventQueue *events) : m_events(events)
{
  if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
    LOG_WARN("BLE listen socket constructed off the main thread; moving ctx to main thread");
  }
  m_ctx = new BlePeripheralContext(this);
  m_ctx->moveToThread(QCoreApplication::instance()->thread());
}

BleListenSocket::~BleListenSocket()
{
  if (m_ctx) {
    m_ctx->stop();
    m_ctx->deleteLater();
    m_ctx = nullptr;
  }
}

void BleListenSocket::bind(const NetworkAddress &)
{
  LOG_NOTE("BleListenSocket::bind");
  auto *app = QCoreApplication::instance();
  const bool sameThread = (app && QThread::currentThread() == app->thread());
  auto conn = sameThread ? Qt::DirectConnection : Qt::BlockingQueuedConnection;
  QMetaObject::invokeMethod(m_ctx, [this] { m_ctx->start(); }, conn);
}

void BleListenSocket::close()
{
  if (m_ctx) {
    auto *app = QCoreApplication::instance();
    const bool sameThread = (app && QThread::currentThread() == app->thread());
    auto conn = sameThread ? Qt::DirectConnection : Qt::BlockingQueuedConnection;
    QMetaObject::invokeMethod(m_ctx, [this] { m_ctx->stop(); }, conn);
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  m_ready.clear();
}

void *BleListenSocket::getEventTarget() const
{
  return const_cast<BleListenSocket *>(this);
}

std::unique_ptr<IDataSocket> BleListenSocket::accept()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_ready.empty())
    return nullptr;
  auto s = std::move(m_ready.front());
  m_ready.pop_front();
  return s;
}

void BleListenSocket::pushAcceptedSocket(std::unique_ptr<BleSocket> socket)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_ready.push_back(std::move(socket));
}

} // namespace deskflow::ble
