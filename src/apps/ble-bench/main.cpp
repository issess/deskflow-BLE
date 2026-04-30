// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
//
// ble-bench: cross-machine connectivity / latency / bandwidth probe for the
// BleSocket transport. One executable, two roles:
//   ble-bench peripheral          # advertise + accept + echo
//   ble-bench central <code>      # scan + connect + benchmark

#include "arch/Arch.h"
#include "base/Event.h"
#include "base/EventQueue.h"
#include "base/EventTypes.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "ble/BleListenSocket.h"
#include "ble/BleSocket.h"
#include "common/Settings.h"
#include "net/IDataSocket.h"
#include "net/NetworkAddress.h"

#include <QCoreApplication>
#include <QDir>
#include <QObject>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace {
// Stdout under SSH/pipe is fully buffered; QTextStream::flush is unreliable
// across Qt versions on Windows when the underlying stream is redirected.
// Drop to printf + explicit fflush for every visible message.
void say(const QString &s)
{
  const QByteArray utf8 = s.toUtf8();
  std::fwrite(utf8.constData(), 1, size_t(utf8.size()), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}
} // namespace

using deskflow::EventTypes;
using namespace std::chrono;

namespace {

constexpr uint8_t kMsgPing = 1;
constexpr uint8_t kMsgPong = 2;
constexpr uint8_t kMsgBenchStart = 3;
constexpr uint8_t kMsgBenchData = 4;
constexpr uint8_t kMsgBenchEnd = 5;
constexpr uint8_t kMsgBenchReport = 6;

QByteArray packU32(uint32_t v)
{
  QByteArray out(4, 0);
  out[0] = char(v & 0xff);
  out[1] = char((v >> 8) & 0xff);
  out[2] = char((v >> 16) & 0xff);
  out[3] = char((v >> 24) & 0xff);
  return out;
}

uint32_t readU32(const char *p)
{
  return uint32_t(uint8_t(p[0])) | (uint32_t(uint8_t(p[1])) << 8) | (uint32_t(uint8_t(p[2])) << 16) |
         (uint32_t(uint8_t(p[3])) << 24);
}

// BleSocket reassembles each write() into one IDataSocket payload, but read()
// returns concatenated payload bytes — there is no message boundary at the
// IStream level. Length-prefix every message so the receiver can split.
QByteArray frameMessage(uint8_t type, const QByteArray &body)
{
  QByteArray msg;
  msg.append(char(type));
  msg.append(body);
  QByteArray framed = packU32(uint32_t(msg.size()));
  framed.append(msg);
  return framed;
}

void writeMessage(IDataSocket *sock, uint8_t type, const QByteArray &body = {})
{
  const QByteArray framed = frameMessage(type, body);
  sock->write(framed.constData(), uint32_t(framed.size()));
}

class MessageReader
{
public:
  std::vector<QByteArray> drain(QByteArray &buf)
  {
    std::vector<QByteArray> out;
    while (true) {
      if (buf.size() < 4)
        return out;
      uint32_t len = readU32(buf.constData());
      if (buf.size() < int(4 + len))
        return out;
      out.emplace_back(buf.mid(4, len));
      buf.remove(0, 4 + len);
    }
  }
};

// Drain and forget any prior pairing state so each run starts fresh.
void initSettings(const QString &mode)
{
  const QString id = QStringLiteral("ble-bench-%1-%2").arg(mode).arg(QCoreApplication::applicationPid());
  Settings::setSettingsFile(QDir::temp().filePath(id + QStringLiteral(".ini")));
  Settings::setStateFile(QDir::temp().filePath(id + QStringLiteral("-state.ini")));
  Settings::setValue(Settings::Server::HasBlePairedPeer, false);
  Settings::setValue(Settings::Client::RemoteBleDevice, QString());
  Settings::setValue(Settings::Client::PendingBleCode, QString());
  Settings::save();
}

// ---------------------- Peripheral ----------------------

class PeripheralBench : public QObject
{
public:
  explicit PeripheralBench(IEventQueue *events) : m_events(events)
  {
  }

  void start()
  {
    initSettings(QStringLiteral("peripheral"));
    m_listen = new deskflow::ble::BleListenSocket(m_events);

    m_events->addHandler(EventTypes::ListenSocketConnecting, m_listen->getEventTarget(), [this](const Event &) {
      onIncoming();
    });

    NetworkAddress dummy;
    m_listen->bind(dummy);
    say(QStringLiteral("ble-bench peripheral: advertising; waiting for central…"));
  }

private:
  void onIncoming()
  {
    auto sock = m_listen->accept();
    if (!sock)
      return;
    m_socket.reset(sock.release());
    m_target = m_socket->getEventTarget();

    m_events->addHandler(EventTypes::DataSocketConnected, m_target, [this](const Event &) { onConnected(); });
    m_events->addHandler(EventTypes::StreamInputReady, m_target, [this](const Event &) { onReadable(); });
    m_events->addHandler(EventTypes::SocketDisconnected, m_target, [this](const Event &) { onDisconnected(); });
  }

  void onConnected()
  {
    say(QStringLiteral("ble-bench peripheral: link up, ready to echo"));
  }

  void onReadable()
  {
    char buf[8192];
    while (true) {
      uint32_t n = m_socket->read(buf, sizeof(buf));
      if (n == 0)
        break;
      m_inbuf.append(buf, int(n));
    }
    auto msgs = m_reader.drain(m_inbuf);
    for (auto &msg : msgs)
      handleMessage(msg);
  }

  void handleMessage(const QByteArray &msg)
  {
    if (msg.isEmpty())
      return;
    const uint8_t t = uint8_t(msg[0]);
    switch (t) {
    case kMsgPing: {
      writeMessage(m_socket.get(), kMsgPong, msg.mid(1));
      break;
    }
    case kMsgBenchStart: {
      m_benchActive = true;
      m_benchBytes = 0;
      m_benchT0 = steady_clock::now();
      say(QStringLiteral("ble-bench peripheral: bandwidth phase started"));
      break;
    }
    case kMsgBenchData: {
      if (m_benchActive)
        m_benchBytes += uint64_t(msg.size() - 1);
      break;
    }
    case kMsgBenchEnd: {
      if (!m_benchActive)
        break;
      const auto t1 = steady_clock::now();
      const double secs = duration<double>(t1 - m_benchT0).count();
      m_benchActive = false;
      say(QString("ble-bench peripheral: received %1 bytes in %2 s = %3 KB/s")
              .arg(m_benchBytes)
              .arg(secs, 0, 'f', 3)
              .arg(m_benchBytes / 1024.0 / std::max(secs, 1e-9), 0, 'f', 2));

      QByteArray body;
      body.append(packU32(uint32_t(m_benchBytes & 0xffffffffu)));
      const uint64_t us = uint64_t(duration_cast<microseconds>(t1 - m_benchT0).count());
      for (int i = 0; i < 8; ++i)
        body.append(char((us >> (8 * i)) & 0xff));
      writeMessage(m_socket.get(), kMsgBenchReport, body);
      break;
    }
    default:
      break;
    }
  }

  void onDisconnected()
  {
    say(QStringLiteral("ble-bench peripheral: link down"));
    QCoreApplication::quit();
  }

  IEventQueue *m_events;
  deskflow::ble::BleListenSocket *m_listen = nullptr;
  std::unique_ptr<IDataSocket> m_socket;
  void *m_target = nullptr;
  QByteArray m_inbuf;
  MessageReader m_reader;
  bool m_benchActive = false;
  uint64_t m_benchBytes = 0;
  steady_clock::time_point m_benchT0;
};

// ---------------------- Central ----------------------

class CentralBench : public QObject
{
public:
  CentralBench(IEventQueue *events, QString code) : m_events(events), m_code(std::move(code))
  {
  }

  void start()
  {
    initSettings(QStringLiteral("central"));
    Settings::setValue(Settings::Client::PendingBleCode, m_code);
    Settings::save();

    m_socket = std::make_unique<deskflow::ble::BleSocket>(m_events);
    m_target = m_socket->getEventTarget();

    m_events->addHandler(EventTypes::DataSocketConnected, m_target, [this](const Event &) { onConnected(); });
    m_events->addHandler(EventTypes::DataSocketConnectionFailed, m_target, [this](const Event &e) {
      auto *info = static_cast<IDataSocket::ConnectionFailedInfo *>(e.getData());
      const QString reason = info ? QString::fromStdString(info->m_what) : QStringLiteral("unknown");
      say(QStringLiteral("ble-bench central: connection failed: ") + reason);
      QCoreApplication::exit(2);
    });
    m_events->addHandler(EventTypes::StreamInputReady, m_target, [this](const Event &) { onReadable(); });
    m_events->addHandler(EventTypes::SocketDisconnected, m_target, [this](const Event &) {
      say(QStringLiteral("ble-bench central: socket disconnected"));
      QCoreApplication::quit();
    });

    NetworkAddress dummy;
    m_socket->connect(dummy);
    say(QStringLiteral("ble-bench central: connecting…"));
  }

private:
  void onConnected()
  {
    say(QStringLiteral("ble-bench central: link up; starting latency phase"));
    QTimer::singleShot(500, this, [this] { startLatencyPhase(); });
  }

  void startLatencyPhase()
  {
    m_pingsSent = 0;
    m_pingSeq = 0;
    m_pingT0.clear();
    m_rtts.clear();
    sendNextPing();
  }

  void sendNextPing()
  {
    if (m_pingsSent >= kPings) {
      QTimer::singleShot(1500, this, [this] { finishLatencyPhase(); });
      return;
    }
    const uint32_t seq = m_pingSeq++;
    QByteArray body(60, 'p');
    body[0] = char(seq & 0xff);
    body[1] = char((seq >> 8) & 0xff);
    body[2] = char((seq >> 16) & 0xff);
    body[3] = char((seq >> 24) & 0xff);
    m_pingT0[seq] = steady_clock::now();
    m_pingPending = seq;
    writeMessage(m_socket.get(), kMsgPing, body);
    ++m_pingsSent;
    // Strict round-trip: do not send the next ping until this one's pong is
    // back. Watchdog drops a missing pong after 4 s so the test can finish.
    QTimer::singleShot(4000, this, [this, seq] {
      if (m_pingPending == seq && m_pingT0.count(seq)) {
        m_pingT0.erase(seq);
        m_pingPending = -1;
        sendNextPing();
      }
    });
  }

  void finishLatencyPhase()
  {
    if (m_rtts.empty()) {
      say(QStringLiteral("ble-bench central: NO PONGS RECEIVED"));
      QCoreApplication::exit(3);
      return;
    }
    auto sorted = m_rtts;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double v : sorted)
      sum += v;
    const double avg = sum / double(sorted.size());
    const double p50 = sorted[sorted.size() / 2];
    const double p90 = sorted[std::min(size_t(double(sorted.size()) * 0.90), sorted.size() - 1)];
    const double p99 = sorted[std::min(size_t(double(sorted.size()) * 0.99), sorted.size() - 1)];
    say(QString("ble-bench central: latency rcvd=%1/%2  min=%3 ms  p50=%4 ms  avg=%5 ms  "
                "p90=%6 ms  p99=%7 ms  max=%8 ms")
            .arg(sorted.size())
            .arg(m_pingsSent)
            .arg(sorted.front(), 0, 'f', 2)
            .arg(p50, 0, 'f', 2)
            .arg(avg, 0, 'f', 2)
            .arg(p90, 0, 'f', 2)
            .arg(p99, 0, 'f', 2)
            .arg(sorted.back(), 0, 'f', 2));

    QTimer::singleShot(500, this, [this] { startBandwidthPhase(); });
  }

  void startBandwidthPhase()
  {
    say(QString("ble-bench central: bandwidth phase (%1 s, %2 B chunks)")
            .arg(kBenchSecs)
            .arg(kBenchChunkBytes));
    writeMessage(m_socket.get(), kMsgBenchStart);
    m_bwSubmitted = 0;
    m_bwT0 = steady_clock::now();
    m_bwActive = true;
    pumpBandwidth();
  }

  void pumpBandwidth()
  {
    if (!m_bwActive)
      return;
    const auto now = steady_clock::now();
    if (duration<double>(now - m_bwT0).count() >= double(kBenchSecs)) {
      m_bwActive = false;
      say(QString("ble-bench central: submitted %1 bytes; waiting for peer report").arg(m_bwSubmitted));
      writeMessage(m_socket.get(), kMsgBenchEnd);
      QTimer::singleShot(15000, this, [this] {
        if (!m_reportSeen) {
          say(QStringLiteral("ble-bench central: bandwidth report not received within 15s"));
          QCoreApplication::exit(4);
        }
      });
      return;
    }
    QByteArray body(kBenchChunkBytes - 1, 'x'); // -1 because type byte adds 1
    writeMessage(m_socket.get(), kMsgBenchData, body);
    m_bwSubmitted += uint64_t(kBenchChunkBytes);
    // Pace just enough for the BleSocket outbound queue not to balloon. The
    // central-side burst-drain in BleSocketContext handles 4 chunks per
    // event-loop turn; we submit one application message (which may fragment
    // into multiple BLE chunks) per turn.
    QTimer::singleShot(5, this, [this] { pumpBandwidth(); });
  }

  void onReadable()
  {
    char buf[8192];
    while (true) {
      uint32_t n = m_socket->read(buf, sizeof(buf));
      if (n == 0)
        break;
      m_inbuf.append(buf, int(n));
    }
    auto msgs = m_reader.drain(m_inbuf);
    for (auto &msg : msgs)
      handleMessage(msg);
  }

  void handleMessage(const QByteArray &msg)
  {
    if (msg.isEmpty())
      return;
    const uint8_t t = uint8_t(msg[0]);
    switch (t) {
    case kMsgPong: {
      if (msg.size() < 5)
        return;
      const uint32_t seq = uint32_t(uint8_t(msg[1])) | (uint32_t(uint8_t(msg[2])) << 8) |
                           (uint32_t(uint8_t(msg[3])) << 16) | (uint32_t(uint8_t(msg[4])) << 24);
      auto it = m_pingT0.find(seq);
      if (it == m_pingT0.end())
        return;
      const auto rtt_ms = duration<double, std::milli>(steady_clock::now() - it->second).count();
      m_rtts.push_back(rtt_ms);
      m_pingT0.erase(it);
      if (int64_t(seq) == m_pingPending) {
        m_pingPending = -1;
        // Schedule next ping on the Qt thread so we don't recurse from the
        // event-queue handler thread.
        QTimer::singleShot(0, this, [this] { sendNextPing(); });
      }
      break;
    }
    case kMsgBenchReport: {
      m_reportSeen = true;
      if (msg.size() < 1 + 4 + 8)
        return;
      const uint32_t bytes = readU32(msg.constData() + 1);
      uint64_t us = 0;
      for (int i = 0; i < 8; ++i)
        us |= uint64_t(uint8_t(msg[1 + 4 + i])) << (8 * i);
      const double secs = double(us) / 1e6;
      const double kbps = double(bytes) / 1024.0 / std::max(secs, 1e-9);
      say(QString("ble-bench central: BANDWIDTH report: peer received %1 bytes in %2 s = %3 KB/s")
              .arg(bytes)
              .arg(secs, 0, 'f', 3)
              .arg(kbps, 0, 'f', 2));
      say(QString("ble-bench central: SUBMITTED %1 bytes from this side").arg(m_bwSubmitted));
      QTimer::singleShot(500, this, [] { QCoreApplication::quit(); });
      break;
    }
    default:
      break;
    }
  }

  static constexpr int kPings = 30;
  static constexpr int kBenchSecs = 5;
  static constexpr int kBenchChunkBytes = 1024;

  IEventQueue *m_events;
  QString m_code;
  std::unique_ptr<deskflow::ble::BleSocket> m_socket;
  void *m_target = nullptr;
  QByteArray m_inbuf;
  MessageReader m_reader;

  int m_pingsSent = 0;
  uint32_t m_pingSeq = 0;
  int64_t m_pingPending = -1;
  std::map<uint32_t, steady_clock::time_point> m_pingT0;
  std::vector<double> m_rtts;

  bool m_bwActive = false;
  steady_clock::time_point m_bwT0;
  uint64_t m_bwSubmitted = 0;
  bool m_reportSeen = false;
};

void usage(const char *exe)
{
  say(QString("Usage:\n  %1 peripheral             advertise + accept + echo\n"
              "  %1 central <code>          scan + connect + benchmark")
          .arg(QString::fromLocal8Bit(exe)));
}

} // namespace

int main(int argc, char **argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  QCoreApplication app(argc, argv);

  Arch arch;
  arch.init();
  Log log;

  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }
  const QString mode = QString::fromLocal8Bit(argv[1]).toLower();

  EventQueue events;

  std::unique_ptr<PeripheralBench> peri;
  std::unique_ptr<CentralBench> cen;
  if (mode == QStringLiteral("peripheral")) {
    peri = std::make_unique<PeripheralBench>(&events);
    QTimer::singleShot(0, [&] { peri->start(); });
  } else if (mode == QStringLiteral("central")) {
    if (argc < 3) {
      usage(argv[0]);
      return 1;
    }
    cen = std::make_unique<CentralBench>(&events, QString::fromLocal8Bit(argv[2]));
    QTimer::singleShot(0, [&] { cen->start(); });
  } else {
    usage(argv[0]);
    return 1;
  }

  // deskflow's IEventQueue runs on a worker thread; Qt's event loop runs on the
  // main thread (BleSocket / BleListenSocket contexts live there).
  std::atomic_bool quitRequested{false};
  std::thread eventThread([&] { events.loop(); });

  const int rc = app.exec();

  // Ask the event-queue loop to exit.
  events.addEvent(Event(EventTypes::Quit));
  if (eventThread.joinable())
    eventThread.join();
  (void)quitRequested;

  return rc;
}
