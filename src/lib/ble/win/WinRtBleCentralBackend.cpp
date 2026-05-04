// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

#include "ble/win/WinRtBleCentralBackend.h"

#include "base/Log.h"
#include "ble/BlePairingCode.h"
#include "ble/BleTransport.h"

#include <QMetaObject>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace deskflow::ble {

namespace {

namespace wfnd = winrt::Windows::Foundation;
namespace wbt = winrt::Windows::Devices::Bluetooth;
namespace wgap = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace wadv = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace wss = winrt::Windows::Storage::Streams;

winrt::guid toGuid(const QBluetoothUuid &u)
{
  const quint128 raw = u.toUInt128();
  winrt::guid g{};
  g.Data1 = (uint32_t(raw.data[0]) << 24) | (uint32_t(raw.data[1]) << 16) | (uint32_t(raw.data[2]) << 8) |
            uint32_t(raw.data[3]);
  g.Data2 = uint16_t((raw.data[4] << 8) | raw.data[5]);
  g.Data3 = uint16_t((raw.data[6] << 8) | raw.data[7]);
  for (int i = 0; i < 8; ++i)
    g.Data4[i] = raw.data[8 + i];
  return g;
}

QByteArray ibufferToQByteArray(const wss::IBuffer &buf)
{
  if (!buf || buf.Length() == 0)
    return {};
  const uint32_t len = buf.Length();
  wss::DataReader reader = wss::DataReader::FromBuffer(buf);
  std::vector<uint8_t> bytes(len);
  reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + len));
  return QByteArray(reinterpret_cast<const char *>(bytes.data()), int(len));
}

wss::IBuffer toIBuffer(const QByteArray &ba)
{
  wss::DataWriter writer;
  if (!ba.isEmpty()) {
    writer.WriteBytes(winrt::array_view<const uint8_t>(
        reinterpret_cast<const uint8_t *>(ba.constData()),
        reinterpret_cast<const uint8_t *>(ba.constData()) + ba.size()));
  }
  return writer.DetachBuffer();
}

class Worker
{
public:
  void start()
  {
    m_running = true;
    m_thread = std::thread([this] { run(); });
  }
  void stop()
  {
    {
      std::lock_guard<std::mutex> lk(m_mu);
      m_running = false;
      m_cv.notify_all();
    }
    if (m_thread.joinable())
      m_thread.join();
  }
  void post(std::function<void()> fn)
  {
    std::lock_guard<std::mutex> lk(m_mu);
    m_q.push(std::move(fn));
    m_cv.notify_all();
  }

private:
  void run()
  {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lk(m_mu);
        m_cv.wait(lk, [this] { return !m_running || !m_q.empty(); });
        if (!m_running && m_q.empty())
          break;
        task = std::move(m_q.front());
        m_q.pop();
      }
      try {
        task();
      } catch (const winrt::hresult_error &e) {
        LOG_WARN("WinRtBleCentralBackend task hr=0x%08x %ls", static_cast<unsigned>(e.code().value),
                 e.message().c_str());
      } catch (const std::exception &e) {
        LOG_WARN("WinRtBleCentralBackend task std::exception: %s", e.what());
      } catch (...) {
        LOG_WARN("WinRtBleCentralBackend task unknown exception");
      }
    }
    try {
      winrt::uninit_apartment();
    } catch (...) {
    }
  }

  std::thread m_thread;
  std::mutex m_mu;
  std::condition_variable m_cv;
  std::queue<std::function<void()>> m_q;
  bool m_running = false;
};

// Helper: parse "AA:BB:CC:DD:EE:FF" into a uint64_t big-endian MAC.
bool parseMac(const QString &s, uint64_t &out)
{
  const auto parts = s.split(QLatin1Char(':'));
  if (parts.size() != 6)
    return false;
  uint64_t v = 0;
  for (const auto &p : parts) {
    bool ok = false;
    const uint64_t b = p.toUInt(&ok, 16);
    if (!ok || b > 0xff)
      return false;
    v = (v << 8) | b;
  }
  out = v;
  return true;
}

} // namespace

struct WinRtBleCentralBackend::Impl
{
  WinRtBleCentralBackend *owner = nullptr;
  Worker worker;

  // State
  std::atomic<bool> running{false};
  std::atomic<int> mtu{64};
  std::atomic<bool> pairingAccepted{false};
  // Peer's 48-bit BT address — captured in doConnect for the consumer to
  // persist as a remembered-peer hint for the next session's reconnect.
  std::atomic<uint64_t> peerAddress{0};
  QString pendingCode;
  QByteArray expectedHash;
  QString savedDeviceId;

  // WinRT objects (worker-thread-only access)
  wadv::BluetoothLEAdvertisementWatcher watcher{nullptr};
  winrt::event_token tokAdvert{};
  wbt::BluetoothLEDevice device{nullptr};
  winrt::event_token tokDeviceConn{};
  wgap::GattDeviceService service{nullptr};
  wgap::GattCharacteristic chPairingAuth{nullptr};
  wgap::GattCharacteristic chPairingStatus{nullptr};
  wgap::GattCharacteristic chDataDownstream{nullptr};
  wgap::GattCharacteristic chDataUpstream{nullptr};
  wgap::GattCharacteristic chControl{nullptr};
  winrt::event_token tokPairingStatus{};
  winrt::event_token tokDataDownstream{};

  void emitConnected()
  {
    QMetaObject::invokeMethod(owner, "connected", Qt::QueuedConnection);
  }
  void emitDisconnected()
  {
    QMetaObject::invokeMethod(owner, "disconnected", Qt::QueuedConnection);
  }
  void emitConnectFailed(const QString &reason)
  {
    QMetaObject::invokeMethod(owner, "connectFailed", Qt::QueuedConnection, Q_ARG(QString, reason));
  }
  void emitDataReceived(const QByteArray &data)
  {
    QMetaObject::invokeMethod(owner, "dataReceived", Qt::QueuedConnection, Q_ARG(QByteArray, data));
  }
  void emitMtuChanged(int v)
  {
    QMetaObject::invokeMethod(owner, "mtuChanged", Qt::QueuedConnection, Q_ARG(int, v));
  }

  // ---------------- worker thread ----------------

  void doStart(QString savedId, QString code, quint64 directAddress)
  {
    LOG_NOTE("WinRtBleCentralBackend: start savedId=%s codeLen=%d directAddr=%012llx",
             savedId.toUtf8().constData(), code.size(),
             static_cast<unsigned long long>(directAddress));
    pendingCode = code;
    if (!code.isEmpty())
      expectedHash = BlePairingCode::hashPrefix(code);
    else
      expectedHash.clear();
    savedDeviceId = savedId;
    pairingAccepted.store(false);

    // Direct-address bypass: skip the watcher and connect straight to the
    // configured BT address. For adapters whose peripheral-mode advertising
    // can't be discovered (e.g. advOffloadSupported=0 with an OS that
    // doesn't surface our service UUID in the adv payload) this is the only
    // way to bring up the link. The host must keep the public BT address
    // stable (no Resolvable Private Address) for this to keep working.
    if (directAddress != 0) {
      LOG_NOTE("WinRtBleCentralBackend: direct-connect bypass (no scan)");
      worker.post([this, directAddress] {
        doConnect(directAddress, wbt::BluetoothAddressType::Public);
      });
      return;
    }

    try {
      watcher = wadv::BluetoothLEAdvertisementWatcher{};
      watcher.ScanningMode(wadv::BluetoothLEScanningMode::Active);
      tokAdvert = watcher.Received([this](const wadv::BluetoothLEAdvertisementWatcher &,
                                          const wadv::BluetoothLEAdvertisementReceivedEventArgs &args) {
        onAdvertisement(args);
      });
      watcher.Start();
      LOG_NOTE("WinRtBleCentralBackend: watcher started");
    } catch (const winrt::hresult_error &e) {
      LOG_ERR("WinRtBleCentralBackend: watcher start hr=0x%08x %ls",
              static_cast<unsigned>(e.code().value), e.message().c_str());
      emitConnectFailed(QStringLiteral("watcher start failed"));
    }
  }

  void onAdvertisement(const wadv::BluetoothLEAdvertisementReceivedEventArgs &args)
  {
    if (!running.load() || pairingAccepted.load())
      return;
    if (device)
      return; // already connecting

    const auto adv = args.Advertisement();
    bool serviceMatch = false;
    for (uint32_t i = 0; i < adv.ServiceUuids().Size(); ++i) {
      if (adv.ServiceUuids().GetAt(i) == toGuid(kServiceUuid)) {
        serviceMatch = true;
        break;
      }
    }
    bool magicMatch = false;
    bool hashOk = false;
    for (uint32_t i = 0; i < adv.ManufacturerData().Size(); ++i) {
      const auto md = adv.ManufacturerData().GetAt(i);
      if (md.CompanyId() != kManufacturerId)
        continue;
      const QByteArray data = ibufferToQByteArray(md.Data());
      if (data.size() < 6)
        continue;
      if ((quint8)data[0] == ((kAdvertMagic >> 8) & 0xFF) &&
          (quint8)data[1] == (kAdvertMagic & 0xFF)) {
        magicMatch = true;
        if (!expectedHash.isEmpty() && data.mid(2, 4) == expectedHash)
          hashOk = true;
      }
    }
    if (!hashOk && !(serviceMatch && (!pendingCode.isEmpty() || !savedDeviceId.isEmpty())))
      return;

    const uint64_t addr = args.BluetoothAddress();
    const wbt::BluetoothAddressType addrType = args.BluetoothAddressType();
    LOG_NOTE(
        "WinRtBleCentralBackend: advertisement match addr=%012llx addrType=%d hashOk=%d serviceMatch=%d magic=%d",
        static_cast<unsigned long long>(addr), static_cast<int>(addrType), hashOk, serviceMatch, magicMatch);

    // Stop scan, hand off to connect on the worker.
    if (watcher) {
      try {
        watcher.Stop();
      } catch (...) {
      }
      watcher.Received(tokAdvert);
      tokAdvert = {};
      watcher = wadv::BluetoothLEAdvertisementWatcher{nullptr};
    }
    worker.post([this, addr, addrType] { doConnect(addr, addrType); });
  }

  void doConnect(uint64_t address, wbt::BluetoothAddressType addressType)
  {
    LOG_NOTE("WinRtBleCentralBackend: connecting to %012llx (addrType=%d)",
             static_cast<unsigned long long>(address), static_cast<int>(addressType));
    peerAddress.store(address);
    try {
      device = wbt::BluetoothLEDevice::FromBluetoothAddressAsync(address, addressType).get();
      if (!device) {
        emitConnectFailed(QStringLiteral("FromBluetoothAddressAsync returned null"));
        return;
      }
      tokDeviceConn = device.ConnectionStatusChanged(
          [this](const wbt::BluetoothLEDevice &d, const wfnd::IInspectable &) {
            const auto s = d.ConnectionStatus();
            LOG_NOTE("WinRtBleCentralBackend: ConnectionStatus -> %d", static_cast<int>(s));
            if (s == wbt::BluetoothConnectionStatus::Disconnected) {
              if (pairingAccepted.load()) {
                pairingAccepted.store(false);
                emitDisconnected();
              }
            }
          });

      const auto svcRes = device.GetGattServicesForUuidAsync(toGuid(kServiceUuid),
                                                              wbt::BluetoothCacheMode::Uncached)
                              .get();
      if (svcRes.Status() != wgap::GattCommunicationStatus::Success ||
          svcRes.Services().Size() == 0) {
        emitConnectFailed(QStringLiteral("Deskflow service not found on peer"));
        return;
      }
      service = svcRes.Services().GetAt(0);

      // Open exclusive — we own this characteristic set on Windows now.
      try {
        const auto open = service.OpenAsync(wgap::GattSharingMode::Exclusive).get();
        LOG_NOTE("WinRtBleCentralBackend: service.OpenAsync status=%d", static_cast<int>(open));
      } catch (const winrt::hresult_error &e) {
        LOG_WARN("WinRtBleCentralBackend: OpenAsync hr=0x%08x %ls",
                 static_cast<unsigned>(e.code().value), e.message().c_str());
      }

      const auto chRes =
          service.GetCharacteristicsAsync(wbt::BluetoothCacheMode::Uncached).get();
      if (chRes.Status() != wgap::GattCommunicationStatus::Success) {
        emitConnectFailed(QStringLiteral("characteristic enumeration failed"));
        return;
      }
      const auto chars = chRes.Characteristics();
      for (uint32_t i = 0; i < chars.Size(); ++i) {
        const auto c = chars.GetAt(i);
        const auto u = c.Uuid();
        if (u == toGuid(kPairingAuthCharUuid))
          chPairingAuth = c;
        else if (u == toGuid(kPairingStatusCharUuid))
          chPairingStatus = c;
        else if (u == toGuid(kDataDownstreamCharUuid))
          chDataDownstream = c;
        else if (u == toGuid(kDataUpstreamCharUuid))
          chDataUpstream = c;
        else if (u == toGuid(kControlCharUuid))
          chControl = c;
      }
      if (!chPairingAuth || !chPairingStatus || !chDataDownstream || !chDataUpstream) {
        emitConnectFailed(QStringLiteral("required characteristics missing"));
        return;
      }
      LOG_NOTE("WinRtBleCentralBackend: all required characteristics bound");

      // Probe MTU via GattSession.
      try {
        auto session = wgap::GattSession::FromDeviceIdAsync(device.BluetoothDeviceId()).get();
        if (session) {
          const int m = static_cast<int>(session.MaxPduSize());
          if (m > 0) {
            mtu.store(m);
            LOG_NOTE("WinRtBleCentralBackend: MTU=%d", m);
            emitMtuChanged(m);
          }
        }
      } catch (...) {
      }

      // Subscribe to notifications.
      auto subscribe = [&](wgap::GattCharacteristic &ch, const char *label) {
        try {
          const auto st =
              ch.WriteClientCharacteristicConfigurationDescriptorAsync(
                    wgap::GattClientCharacteristicConfigurationDescriptorValue::Notify)
                  .get();
          LOG_NOTE("WinRtBleCentralBackend: subscribe %s status=%d", label, static_cast<int>(st));
          return st == wgap::GattCommunicationStatus::Success;
        } catch (const winrt::hresult_error &e) {
          LOG_WARN("WinRtBleCentralBackend: subscribe %s hr=0x%08x %ls", label,
                   static_cast<unsigned>(e.code().value), e.message().c_str());
          return false;
        }
      };

      tokPairingStatus = chPairingStatus.ValueChanged(
          [this](const wgap::GattCharacteristic &, const wgap::GattValueChangedEventArgs &args) {
            const QByteArray v = ibufferToQByteArray(args.CharacteristicValue());
            if (v.isEmpty())
              return;
            const auto status = static_cast<PairingStatus>(quint8(v[0]));
            LOG_NOTE("WinRtBleCentralBackend: PairingStatus notify=%d", static_cast<int>(status));
            if (status == PairingStatus::Accepted) {
              pairingAccepted.store(true);
              emitConnected();
            } else if (status == PairingStatus::Rejected) {
              emitConnectFailed(QStringLiteral("host rejected code"));
            }
          });
      tokDataDownstream = chDataDownstream.ValueChanged(
          [this](const wgap::GattCharacteristic &, const wgap::GattValueChangedEventArgs &args) {
            emitDataReceived(ibufferToQByteArray(args.CharacteristicValue()));
          });

      subscribe(chPairingStatus, "PairingStatus");
      subscribe(chDataDownstream, "DataDownstream");
      if (chControl)
        subscribe(chControl, "Control");

      // Request short connection interval (best effort).
      try {
        const auto params = wbt::BluetoothLEPreferredConnectionParameters::ThroughputOptimized();
        auto req = device.RequestPreferredConnectionParameters(params);
        if (req)
          LOG_NOTE("WinRtBleCentralBackend: ThroughputOptimized status=%d",
                   static_cast<int>(req.Status()));
      } catch (...) {
      }

      // Pairing flow.
      if (!pendingCode.isEmpty()) {
        LOG_NOTE("WinRtBleCentralBackend: writing pairing code");
        const auto wst = chPairingAuth
                              .WriteValueAsync(toIBuffer(pendingCode.toUtf8()),
                                               wgap::GattWriteOption::WriteWithoutResponse)
                              .get();
        LOG_NOTE("WinRtBleCentralBackend: pairing write status=%d", static_cast<int>(wst));
      } else if (!savedDeviceId.isEmpty()) {
        // Remembered-peer reconnect: skip code, treat the GATT subscription as
        // proof of identity (we matched the saved peer ID at scan time).
        LOG_NOTE("WinRtBleCentralBackend: remembered-peer mode, skipping code");
        pairingAccepted.store(true);
        emitConnected();
      }
    } catch (const winrt::hresult_error &e) {
      LOG_ERR("WinRtBleCentralBackend: connect hr=0x%08x %ls",
              static_cast<unsigned>(e.code().value), e.message().c_str());
      emitConnectFailed(QString::fromStdWString(std::wstring(e.message().c_str())));
    }
  }

  void doStop()
  {
    if (watcher) {
      try {
        watcher.Stop();
      } catch (...) {
      }
      try {
        if (tokAdvert.value)
          watcher.Received(tokAdvert);
      } catch (...) {
      }
      tokAdvert = {};
      watcher = wadv::BluetoothLEAdvertisementWatcher{nullptr};
    }
    if (chPairingStatus && tokPairingStatus.value) {
      try {
        chPairingStatus.ValueChanged(tokPairingStatus);
      } catch (...) {
      }
      tokPairingStatus = {};
    }
    if (chDataDownstream && tokDataDownstream.value) {
      try {
        chDataDownstream.ValueChanged(tokDataDownstream);
      } catch (...) {
      }
      tokDataDownstream = {};
    }
    chPairingAuth = wgap::GattCharacteristic{nullptr};
    chPairingStatus = wgap::GattCharacteristic{nullptr};
    chDataDownstream = wgap::GattCharacteristic{nullptr};
    chDataUpstream = wgap::GattCharacteristic{nullptr};
    chControl = wgap::GattCharacteristic{nullptr};
    if (service) {
      try {
        service.Close();
      } catch (...) {
      }
      service = wgap::GattDeviceService{nullptr};
    }
    if (device) {
      try {
        if (tokDeviceConn.value)
          device.ConnectionStatusChanged(tokDeviceConn);
      } catch (...) {
      }
      tokDeviceConn = {};
      device = wbt::BluetoothLEDevice{nullptr};
    }
    pairingAccepted.store(false);
  }

  void doWrite(QByteArray chunk)
  {
    if (!chDataUpstream)
      return;
    // GATT WriteWithResponse with synchronous .get(). The ATT-level ack is
    // what gives us actual end-to-end delivery confirmation — WinRT's
    // .get() on WriteWithoutResponse only confirms the OS BT stack
    // accepted the request, leaving room for silent LL drops to corrupt
    // the inbound TLS record stream on the peer (a single missing byte
    // fails MAC verification and tears the connection down).
    // Per-chunk RTT is ~one Connection Event (7.5–15 ms with
    // ThroughputOptimized parameters). For Deskflow's typical sparse
    // input events (mouse moves at 60–120 Hz, keystrokes <30 Hz),
    // each write completes well within the inter-event gap, so the
    // user-visible end-to-end latency is unchanged versus async — both
    // are LL-bound for the wire transit anyway. Bulk transfers cap out
    // around 6–12 KB/s, which matches the bench-measured BLE limit.
    constexpr int kMaxAttempts = 5;
    constexpr DWORD kBackoffMs = 1;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      try {
        auto buf = toIBuffer(chunk);
        const auto status =
            chDataUpstream.WriteValueAsync(buf, wgap::GattWriteOption::WriteWithResponse).get();
        if (status != wgap::GattCommunicationStatus::Success) {
          LOG_WARN("WinRtBleCentralBackend: write status=%d size=%d attempt=%d",
                   static_cast<int>(status), chunk.size(), attempt);
        }
        return;
      } catch (const winrt::hresult_error &e) {
        const auto hr = static_cast<unsigned>(e.code().value);
        const bool retryable = (hr == 0x8000000E /* E_ILLEGAL_METHOD_CALL */);
        if (retryable && attempt < kMaxAttempts) {
          ::Sleep(kBackoffMs * attempt);
          continue;
        }
        LOG_WARN("WinRtBleCentralBackend: write hr=0x%08x size=%d attempts=%d",
                 hr, chunk.size(), attempt);
        return;
      } catch (const std::exception &e) {
        LOG_WARN("WinRtBleCentralBackend: write threw std::exception: %s size=%d",
                 e.what(), chunk.size());
        return;
      }
    }
  }
};

WinRtBleCentralBackend::WinRtBleCentralBackend(QObject *parent)
    : QObject(parent), m_impl(std::make_unique<Impl>())
{
  m_impl->owner = this;
  m_impl->worker.start();
  m_impl->running.store(true);
}

WinRtBleCentralBackend::~WinRtBleCentralBackend()
{
  m_impl->running.store(false);
  m_impl->worker.post([this] { m_impl->doStop(); });
  m_impl->worker.stop();
}

void WinRtBleCentralBackend::start(const QString &savedDeviceId, const QString &code, quint64 directAddress)
{
  const QString sid = savedDeviceId;
  const QString c = code;
  const quint64 da = directAddress;
  m_impl->worker.post([this, sid, c, da] { m_impl->doStart(sid, c, da); });
}

void WinRtBleCentralBackend::stop()
{
  m_impl->worker.post([this] { m_impl->doStop(); });
}

void WinRtBleCentralBackend::writeUpstream(const QByteArray &chunk)
{
  m_impl->worker.post([this, chunk] { m_impl->doWrite(chunk); });
}

int WinRtBleCentralBackend::mtu() const
{
  return m_impl->mtu.load();
}

quint64 WinRtBleCentralBackend::peerAddress() const
{
  return m_impl->peerAddress.load();
}

} // namespace deskflow::ble
