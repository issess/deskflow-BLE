/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ble/win/WinRtBlePeripheralBackend.h"

#include "base/Log.h"
#include "ble/BleProtocolClassifier.h"
#include "ble/BleTransport.h"

#include <QMetaObject>
#include <QThread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace deskflow::ble {

namespace {

namespace wfnd = winrt::Windows::Foundation;
namespace wfc = winrt::Windows::Foundation::Collections;
namespace wbt = winrt::Windows::Devices::Bluetooth;
namespace wgap = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace wadv = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace wss = winrt::Windows::Storage::Streams;

winrt::guid toGuid(const QBluetoothUuid &u)
{
  const quint128 raw = u.toUInt128();
  winrt::guid g{};
  g.Data1 = (static_cast<uint32_t>(raw.data[0]) << 24) | (static_cast<uint32_t>(raw.data[1]) << 16) |
            (static_cast<uint32_t>(raw.data[2]) << 8) | static_cast<uint32_t>(raw.data[3]);
  g.Data2 = static_cast<uint16_t>((raw.data[4] << 8) | raw.data[5]);
  g.Data3 = static_cast<uint16_t>((raw.data[6] << 8) | raw.data[7]);
  for (int i = 0; i < 8; ++i)
    g.Data4[i] = raw.data[8 + i];
  return g;
}

QByteArray ibufferToQByteArray(const wss::IBuffer &buf)
{
  if (!buf)
    return {};
  const uint32_t len = buf.Length();
  if (len == 0)
    return {};
  wss::DataReader reader = wss::DataReader::FromBuffer(buf);
  std::vector<uint8_t> bytes(len);
  reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + len));
  return QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<int>(len));
}

wss::IBuffer qByteArrayToIBuffer(const QByteArray &ba)
{
  wss::DataWriter writer;
  if (!ba.isEmpty()) {
    writer.WriteBytes(
        winrt::array_view<const uint8_t>(reinterpret_cast<const uint8_t *>(ba.constData()),
                                         reinterpret_cast<const uint8_t *>(ba.constData()) + ba.size()));
  }
  return writer.DetachBuffer();
}

} // namespace

// Worker thread that owns all WinRT objects. It runs its own serialized
// task queue so we never touch WinRT from the Qt main (STA) thread.
class WinRtWorker
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
      std::lock_guard<std::mutex> lock(m_mu);
      m_running = false;
      m_cv.notify_all();
    }
    if (m_thread.joinable())
      m_thread.join();
  }

  void post(std::function<void()> fn)
  {
    std::lock_guard<std::mutex> lock(m_mu);
    m_q.push(std::move(fn));
    m_cv.notify_all();
  }

private:
  void run()
  {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      LOG_NOTE("WinRT worker: MTA apartment initialised");
    } catch (const winrt::hresult_error &e) {
      LOG_WARN("WinRT worker: init_apartment threw hr=0x%08x (continuing)",
               static_cast<unsigned>(e.code().value));
    }

    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(m_mu);
        m_cv.wait(lock, [this] { return !m_running || !m_q.empty(); });
        if (!m_running && m_q.empty())
          break;
        task = std::move(m_q.front());
        m_q.pop();
      }
      try {
        task();
      } catch (const winrt::hresult_error &e) {
        LOG_ERR("WinRT worker: task threw hr=0x%08x msg=%ls",
                static_cast<unsigned>(e.code().value), e.message().c_str());
      } catch (const std::exception &e) {
        LOG_ERR("WinRT worker: task threw std::exception: %s", e.what());
      } catch (...) {
        LOG_ERR("WinRT worker: task threw unknown exception");
      }
    }

    try {
      winrt::uninit_apartment();
    } catch (...) {
    }
    LOG_NOTE("WinRT worker: thread exiting");
  }

  std::thread m_thread;
  std::mutex m_mu;
  std::condition_variable m_cv;
  std::queue<std::function<void()>> m_q;
  bool m_running = false;
};

struct WinRtBlePeripheralBackend::Impl
{
  WinRtBlePeripheralBackend *owner = nullptr;
  WinRtWorker worker;

  wgap::GattServiceProvider serviceProvider{nullptr};
  wgap::GattLocalCharacteristic pairingAuth{nullptr};
  wgap::GattLocalCharacteristic pairingStatus{nullptr};
  wgap::GattLocalCharacteristic dataDownstream{nullptr};
  wgap::GattLocalCharacteristic dataUpstream{nullptr};
  wgap::GattLocalCharacteristic control{nullptr};

  // Companion advertiser carrying the Deskflow manufacturer-data blob in
  // parallel with the GattServiceProvider's advertising. The GATT advertising
  // params object only exposes IsConnectable / IsDiscoverable; it cannot
  // include manufacturer data. Without this publisher, peers that filter by
  // manufacturer-data magic would have to fall back to UUID-only matching.
  wadv::BluetoothLEAdvertisementPublisher mfgPublisher{nullptr};
  winrt::event_token tokMfgPublisherStatus{};

  // Holds the result of BluetoothLEDevice::RequestPreferredConnectionParameters.
  // Windows keeps the requested interval in effect only while this object is
  // alive — releasing it reverts the link to default parameters. We grab one
  // when the first central subscribes and drop it on disconnect.
  wbt::BluetoothLEPreferredConnectionParametersRequest preferredConnReq{nullptr};

  winrt::event_token tokPairingAuthWrite{};
  winrt::event_token tokDataUpstreamWrite{};
  winrt::event_token tokAdvertisementStatusChanged{};
  winrt::event_token tokSubscribedChangedStatus{};
  winrt::event_token tokSubscribedChangedDownstream{};

  std::atomic<bool> started{false};
  std::atomic<bool> starting{false};
  std::atomic<int> pairingStatusSubscribers{0};
  std::atomic<int> downstreamSubscribers{0};
  std::atomic<int> mtu{64};
  QString localName;
  QByteArray mfgPayload;

  // Bounded outbound queue for DataDownstream notifies. Producer-side guard:
  // if the upper layer pushes faster than the BLE link can drain, we coalesce
  // mouse-move chunks first, then drop oldest on overflow so critical frames
  // (keep-alives, key events) aren't stuck behind stale mouse positions.
  // The cap is generous because pipelined notifies (see kDownstreamInflightWindow)
  // drain the queue at link rate; the cap only matters during transient bursts.
  static constexpr size_t kDownstreamQueueCap = 64;
  // Outstanding NotifyValueAsync ops we keep in flight to the OS BT stack.
  // The stack pipelines these to the link layer at hardware speed; serialising
  // each via .get() instead of letting them overlap was capping throughput at
  // ~12 KB/s. 16 in flight saturates the 1 Mbps PHY without unbounded host
  // queueing.
  static constexpr int kDownstreamInflightWindow = 16;
  std::mutex dsMu;
  std::deque<QByteArray> dsQueue;
  bool dsPumpScheduled = false;
  std::atomic<uint64_t> dsDroppedTotal{0};
  std::atomic<uint64_t> dsCoalescedMouseTotal{0};
  // Heap-allocated so Completed lambdas can decrement safely even if Impl is
  // torn down before the OS finalises the async op.
  std::shared_ptr<std::atomic<int>> dsInflight = std::make_shared<std::atomic<int>>(0);

  void emitStartFailed(const QString &reason)
  {
    QMetaObject::invokeMethod(owner, "startFailed", Qt::QueuedConnection, Q_ARG(QString, reason));
  }
  void emitStarted()
  {
    QMetaObject::invokeMethod(owner, "started", Qt::QueuedConnection);
  }
  void emitPairingAuthWritten(const QByteArray &v)
  {
    QMetaObject::invokeMethod(owner, "pairingAuthWritten", Qt::QueuedConnection, Q_ARG(QByteArray, v));
  }
  void emitUpstreamWritten(const QByteArray &v)
  {
    QMetaObject::invokeMethod(owner, "upstreamWritten", Qt::QueuedConnection, Q_ARG(QByteArray, v));
  }
  void emitCentralConnected()
  {
    QMetaObject::invokeMethod(owner, "centralConnected", Qt::QueuedConnection);
  }
  void emitCentralDisconnected()
  {
    QMetaObject::invokeMethod(owner, "centralDisconnected", Qt::QueuedConnection);
  }
  void emitMtuChanged(int v)
  {
    QMetaObject::invokeMethod(owner, "mtuChanged", Qt::QueuedConnection, Q_ARG(int, v));
  }

  // Inspect any currently-subscribed client's GATT session and pull its
  // negotiated MaxPduSize. WinRT exposes per-client GattSession with the
  // negotiated ATT MTU; we use the highest value reported across the
  // current subscribers and propagate it up so chunking uses real-MTU
  // sized writes instead of the 23-byte ATT default. Quiet on errors —
  // fallback chunking still works at the default MTU.
  void refreshNegotiatedMtuFromSubscribers()
  {
    auto probe = [this](wgap::GattLocalCharacteristic const &ch) -> int {
      if (!ch)
        return 0;
      int best = 0;
      try {
        auto subs = ch.SubscribedClients();
        for (uint32_t i = 0; i < subs.Size(); ++i) {
          auto sub = subs.GetAt(i);
          auto session = sub.Session();
          if (!session)
            continue;
          int s = static_cast<int>(session.MaxPduSize());
          if (s > best)
            best = s;
        }
      } catch (...) {
      }
      return best;
    };
    const int s1 = probe(pairingStatus);
    const int s2 = probe(dataDownstream);
    const int negotiated = std::max(s1, s2);
    if (negotiated <= 0)
      return;
    const int prev = mtu.load();
    if (negotiated == prev)
      return;
    mtu.store(negotiated);
    LOG_NOTE("WinRT: MTU updated %d -> %d (from peer GattSession)", prev, negotiated);
    emitMtuChanged(negotiated);
    // TestC: link is mature now (post-MTU exchange); the original request issued
    // at SubscribedClientsChanged time often hits PartialFailure because the OS
    // still considers the link unstable. Drop and re-issue once the link is
    // warm — the second request is more likely to be honoured.
    worker.post([this] {
      if (preferredConnReq) {
        preferredConnReq = nullptr;
        LOG_NOTE("WinRT: dropping initial preferred-conn-params request to retry post-MTU");
      }
      requestThroughputOptimizedConn();
    });
  }

  // Ask the link layer for ThroughputOptimized connection params (short
  // interval, no slave latency). The peer is free to refuse or pick a value
  // inside its supported range. Windows keeps the request in effect only
  // while the returned object stays alive, so we cache it on Impl. Best
  // effort — failure leaves the OS-default interval in place.
  void requestThroughputOptimizedConn()
  {
    if (preferredConnReq)
      return; // already requested
    try {
      auto pickSession = [this]() -> wgap::GattSession {
        for (auto &ch : {dataDownstream, pairingStatus}) {
          if (!ch)
            continue;
          auto subs = ch.SubscribedClients();
          for (uint32_t i = 0; i < subs.Size(); ++i) {
            auto sub = subs.GetAt(i);
            auto session = sub.Session();
            if (session)
              return session;
          }
        }
        return wgap::GattSession{nullptr};
      };
      auto session = pickSession();
      if (!session) {
        LOG_DEBUG("WinRT: requestThroughputOptimizedConn — no session yet");
        return;
      }
      auto deviceId = session.DeviceId();
      auto device = wbt::BluetoothLEDevice::FromIdAsync(deviceId.Id()).get();
      if (!device) {
        LOG_WARN("WinRT: BluetoothLEDevice::FromIdAsync returned null for connection-params");
        return;
      }
      auto params = wbt::BluetoothLEPreferredConnectionParameters::ThroughputOptimized();
      preferredConnReq = device.RequestPreferredConnectionParameters(params);
      const int status = preferredConnReq
                             ? static_cast<int>(preferredConnReq.Status())
                             : -1;
      LOG_NOTE("WinRT: requested ThroughputOptimized connection params; status=%d "
               "(0=Success 1=PartialFailure 2=Disabled 3=DisabledByPolicy 4=DisabledForPeer)",
               status);
    } catch (const winrt::hresult_error &e) {
      LOG_WARN("WinRT: RequestPreferredConnectionParameters threw hr=0x%08x msg=%ls",
               static_cast<unsigned>(e.code().value), e.message().c_str());
    } catch (const std::exception &e) {
      LOG_WARN("WinRT: RequestPreferredConnectionParameters threw std::exception: %s", e.what());
    } catch (...) {
      LOG_WARN("WinRT: RequestPreferredConnectionParameters threw unknown exception");
    }
  }

  void releasePreferredConnRequest()
  {
    if (!preferredConnReq)
      return;
    preferredConnReq = nullptr;
    LOG_NOTE("WinRT: released preferred connection params request (link reverts to default)");
  }

  // Runs on the worker (MTA) thread.
  bool workerStart()
  {
    LOG_NOTE("WinRT: worker start() begin");
    bool supportsPeripheral = false;
    // Log local BT adapter address — lets the user compare with what a
    // sniffer / Bluetooth LE Explorer sees over the air.
    try {
      auto adapter = wbt::BluetoothAdapter::GetDefaultAsync().get();
      if (adapter) {
        auto mac = adapter.BluetoothAddress();
        supportsPeripheral = adapter.IsPeripheralRoleSupported();
        LOG_NOTE("WinRT: adapter addr=%02X:%02X:%02X:%02X:%02X:%02X peripheralSupported=%d centralSupported=%d advOffloadSupported=%d",
                 (unsigned)((mac >> 40) & 0xFF), (unsigned)((mac >> 32) & 0xFF),
                 (unsigned)((mac >> 24) & 0xFF), (unsigned)((mac >> 16) & 0xFF),
                 (unsigned)((mac >> 8) & 0xFF), (unsigned)(mac & 0xFF),
                 supportsPeripheral, adapter.IsCentralRoleSupported(),
                 adapter.IsAdvertisementOffloadSupported());
        if (!supportsPeripheral) {
          LOG_WARN("WinRT: adapter reports peripheral role NOT supported; attempting StartAdvertising anyway");
        }
      } else {
        LOG_WARN("WinRT: no default Bluetooth adapter");
      }
    } catch (const winrt::hresult_error &e) {
      LOG_WARN("WinRT: adapter query threw hr=0x%08x",
               static_cast<unsigned>(e.code().value));
    }
    try {
      LOG_NOTE("WinRT: GattServiceProvider::CreateAsync calling");
      auto op = wgap::GattServiceProvider::CreateAsync(toGuid(kServiceUuid));
      auto result = op.get();
      LOG_NOTE("WinRT: CreateAsync returned, error=%d", static_cast<int>(result.Error()));
      if (result.Error() != wbt::BluetoothError::Success) {
        emitStartFailed(
            QStringLiteral("CreateAsync error %1").arg(static_cast<int>(result.Error())));
        return false;
      }
      serviceProvider = result.ServiceProvider();
      LOG_NOTE("WinRT: serviceProvider obtained");
    } catch (const winrt::hresult_error &e) {
      LOG_ERR("WinRT: CreateAsync threw hr=0x%08x msg=%ls",
              static_cast<unsigned>(e.code().value), e.message().c_str());
      emitStartFailed(QStringLiteral("CreateAsync hr=0x%1").arg(
          QString::number(static_cast<quint32>(e.code().value), 16)));
      return false;
    }

    auto addChar = [this](const QBluetoothUuid &uuid,
                          wgap::GattCharacteristicProperties props,
                          const char *label,
                          wgap::GattLocalCharacteristic &out) -> bool {
      try {
        LOG_NOTE("WinRT: CreateCharacteristicAsync(%s) calling", label);
        wgap::GattLocalCharacteristicParameters p;
        p.CharacteristicProperties(props);
        p.ReadProtectionLevel(wgap::GattProtectionLevel::Plain);
        p.WriteProtectionLevel(wgap::GattProtectionLevel::Plain);
        auto op = serviceProvider.Service().CreateCharacteristicAsync(toGuid(uuid), p);
        auto res = op.get();
        LOG_NOTE("WinRT: CreateCharacteristicAsync(%s) returned, error=%d",
                 label, static_cast<int>(res.Error()));
        if (res.Error() != wbt::BluetoothError::Success)
          return false;
        out = res.Characteristic();
        return true;
      } catch (const winrt::hresult_error &e) {
        LOG_ERR("WinRT: CreateCharacteristicAsync(%s) threw hr=0x%08x",
                label, static_cast<unsigned>(e.code().value));
        return false;
      }
    };

    if (!addChar(kPairingAuthCharUuid,
                 wgap::GattCharacteristicProperties::Write |
                     wgap::GattCharacteristicProperties::WriteWithoutResponse,
                 "PairingAuth", pairingAuth)) {
      emitStartFailed(QStringLiteral("PairingAuth characteristic create failed"));
      return false;
    }
    if (!addChar(kPairingStatusCharUuid, wgap::GattCharacteristicProperties::Notify, "PairingStatus",
                 pairingStatus)) {
      emitStartFailed(QStringLiteral("PairingStatus characteristic create failed"));
      return false;
    }
    // DataDownstream uses GATT Notify (fire-and-forget). Loss is detected
    // and resynced at the application layer by BleFraming packet IDs:
    // BleFramingReader::feedChunk drops stale-id chunks and abandons any
    // in-progress reassembly on an id-gap, so a single dropped chunk loses
    // one logical frame instead of desyncing the upper-layer stream parser.
    if (!addChar(kDataDownstreamCharUuid, wgap::GattCharacteristicProperties::Notify,
                 "DataDownstream", dataDownstream)) {
      emitStartFailed(QStringLiteral("DataDownstream characteristic create failed"));
      return false;
    }
    if (!addChar(kDataUpstreamCharUuid,
                 wgap::GattCharacteristicProperties::Write |
                     wgap::GattCharacteristicProperties::WriteWithoutResponse,
                 "DataUpstream", dataUpstream)) {
      emitStartFailed(QStringLiteral("DataUpstream characteristic create failed"));
      return false;
    }
    if (!addChar(kControlCharUuid, wgap::GattCharacteristicProperties::Notify, "Control", control)) {
      emitStartFailed(QStringLiteral("Control characteristic create failed"));
      return false;
    }

    LOG_NOTE("WinRT: wiring events");
    wireEvents();

    LOG_NOTE("WinRT: calling StartAdvertising");
    auto logAttemptResult = [this](const char *label) -> bool {
      const auto status = serviceProvider.AdvertisementStatus();
      LOG_NOTE("WinRT: attempt [%s] returned, status=%d", label, static_cast<int>(status));
      if (status == wgap::GattServiceProviderAdvertisementStatus::Started) {
        if (!started.exchange(true))
          emitStarted();
        return true;
      }
      return false;
    };
    auto logAttemptError = [](const char *label, const winrt::hresult_error &e) {
      LOG_ERR("WinRT: attempt [%s] winrt::hresult_error 0x%08x msg=%ls",
              label, static_cast<unsigned>(e.code().value), e.message().c_str());
    };
    auto attemptStartWithParams = [this, &logAttemptResult, &logAttemptError](
                                      const char *label, bool connectable, bool discoverable) -> bool {
      try {
        LOG_NOTE("WinRT: attempt [%s]", label);
        wgap::GattServiceProviderAdvertisingParameters p;
        p.IsConnectable(connectable);
        p.IsDiscoverable(discoverable);
        serviceProvider.StartAdvertising(p);
        return logAttemptResult(label);
      } catch (const winrt::hresult_error &e) {
        logAttemptError(label, e);
      } catch (const std::exception &e) {
        LOG_ERR("WinRT: attempt [%s] std::exception: %s", label, e.what());
      } catch (...) {
        LOG_ERR("WinRT: attempt [%s] unknown exception", label);
      }
      return false;
    };
    auto attemptStartDefault = [this, &logAttemptResult, &logAttemptError](const char *label) -> bool {
      try {
        LOG_NOTE("WinRT: attempt [%s]", label);
        serviceProvider.StartAdvertising();
        return logAttemptResult(label);
      } catch (const winrt::hresult_error &e) {
        logAttemptError(label, e);
      } catch (const std::exception &e) {
        LOG_ERR("WinRT: attempt [%s] std::exception: %s", label, e.what());
      } catch (...) {
        LOG_ERR("WinRT: attempt [%s] unknown exception", label);
      }
      return false;
    };

    // On adapters that report no peripheral role, the parameter overload can
    // throw from inside C++/WinRT before our attempt-level catch can handle it.
    // Try the default overload only; it is the least invasive stack probe.
    starting = true;
    bool ok = false;
    if (!supportsPeripheral) {
      ok = attemptStartDefault("default-no-args-no-peripheral-role");
    } else {
      // Walk several parameter combinations. Start with the documented GATT
      // server mode; the later variants are diagnostics/fallbacks for stacks
      // that reject one of the flags. Non-connectable advertising cannot carry
      // Deskflow traffic, but its failure mode helps distinguish capability
      // problems from parameter validation problems.
      ok = attemptStartWithParams("connectable+discoverable", true, true);
      if (!ok) {
        ok = attemptStartWithParams("connectable-only", true, false);
      }
      if (!ok && started) {
        ok = true;
      }
      if (!ok) {
        ok = attemptStartWithParams("discoverable-only", false, true);
      }
      if (!ok && started) {
        ok = true;
      }
      if (!ok) {
        ok = attemptStartDefault("default-no-args");
      }
      if (!ok && started) {
        ok = true;
      }
    }
    if (!ok) {
      starting = false;
      emitStartFailed(QStringLiteral(
          "All GattServiceProvider::StartAdvertising variants failed; adapter/driver may not support LE peripheral role"));
      return false;
    }
    starting = false;
    startManufacturerDataAdvertiser();
    return true;
  }

  // Run alongside the GattServiceProvider advertiser. Carries the Deskflow
  // manufacturer-data magic + code-hash so peers can filter on mfgData
  // before bothering to connect/discover. Best-effort: if Windows refuses
  // (e.g. radio busy, adapter doesn't allow concurrent legacy advertisers)
  // we just log and continue — the GATT-level UUID match path still works.
  void startManufacturerDataAdvertiser()
  {
    if (mfgPayload.isEmpty()) {
      LOG_NOTE("WinRT: skipping manufacturer-data advertiser (empty payload)");
      return;
    }
    try {
      mfgPublisher = wadv::BluetoothLEAdvertisementPublisher();
      auto adv = mfgPublisher.Advertisement();

      wadv::BluetoothLEManufacturerData md;
      md.CompanyId(kManufacturerId);
      wss::DataWriter w;
      w.WriteBytes(winrt::array_view<const uint8_t>(
          reinterpret_cast<const uint8_t *>(mfgPayload.constData()),
          reinterpret_cast<const uint8_t *>(mfgPayload.constData()) + mfgPayload.size()));
      md.Data(w.DetachBuffer());
      adv.ManufacturerData().Append(md);

      tokMfgPublisherStatus = mfgPublisher.StatusChanged(
          [](wadv::BluetoothLEAdvertisementPublisher const &sender,
             wadv::BluetoothLEAdvertisementPublisherStatusChangedEventArgs const &args) {
            LOG_NOTE("WinRT: mfgData publisher status -> %d error=%d",
                     static_cast<int>(sender.Status()), static_cast<int>(args.Error()));
          });
      mfgPublisher.Start();
      LOG_NOTE("WinRT: mfgData publisher Start() called (companyId=0x%04x payload=%d bytes)",
               kManufacturerId, mfgPayload.size());
    } catch (const winrt::hresult_error &e) {
      LOG_WARN("WinRT: mfgData publisher Start threw hr=0x%08x msg=%ls — continuing with UUID-only advertising",
               static_cast<unsigned>(e.code().value), e.message().c_str());
      mfgPublisher = nullptr;
    } catch (const std::exception &e) {
      LOG_WARN("WinRT: mfgData publisher Start threw std::exception: %s — continuing", e.what());
      mfgPublisher = nullptr;
    }
  }

  void stopManufacturerDataAdvertiser()
  {
    if (!mfgPublisher)
      return;
    try {
      if (tokMfgPublisherStatus.value)
        mfgPublisher.StatusChanged(tokMfgPublisherStatus);
      tokMfgPublisherStatus = {};
      const auto st = mfgPublisher.Status();
      if (st == wadv::BluetoothLEAdvertisementPublisherStatus::Started ||
          st == wadv::BluetoothLEAdvertisementPublisherStatus::Waiting) {
        mfgPublisher.Stop();
      }
    } catch (...) {
    }
    mfgPublisher = nullptr;
  }

  void wireEvents()
  {
    tokPairingAuthWrite = pairingAuth.WriteRequested(
        [this](wgap::GattLocalCharacteristic const &, wgap::GattWriteRequestedEventArgs const &args) {
          auto deferral = args.GetDeferral();
          try {
            auto req = args.GetRequestAsync().get();
            if (req) {
              QByteArray v = ibufferToQByteArray(req.Value());
              if (req.Option() == wgap::GattWriteOption::WriteWithResponse)
                req.Respond();
              emitPairingAuthWritten(v);
            }
          } catch (...) {
          }
          deferral.Complete();
        });

    tokDataUpstreamWrite = dataUpstream.WriteRequested(
        [this](wgap::GattLocalCharacteristic const &, wgap::GattWriteRequestedEventArgs const &args) {
          auto deferral = args.GetDeferral();
          try {
            auto req = args.GetRequestAsync().get();
            if (req) {
              QByteArray v = ibufferToQByteArray(req.Value());
              if (req.Option() == wgap::GattWriteOption::WriteWithResponse)
                req.Respond();
              emitUpstreamWritten(v);
            }
          } catch (...) {
          }
          deferral.Complete();
        });

    auto updateSubscribers = [this](const char *label, std::atomic<int> &counter,
                                    wgap::GattLocalCharacteristic const &sender) {
      const auto clients = sender.SubscribedClients();
      const int n = static_cast<int>(clients.Size());
      const int prevTotal = pairingStatusSubscribers.load() + downstreamSubscribers.load();
      counter.store(n);
      const int total = pairingStatusSubscribers.load() + downstreamSubscribers.load();
      LOG_NOTE("WinRT: %s subscribed clients count = %d total=%d", label, n, total);
      if (prevTotal == 0 && total > 0) {
        LOG_NOTE("WinRT: emitCentralConnected (prevTotal=0 -> total=%d)", total);
        emitCentralConnected();
        // Defer the connection-params request to the worker thread so all
        // BluetoothLEDevice / GattSession touches happen on a single MTA
        // pump and never race the notify path.
        worker.post([this] { requestThroughputOptimizedConn(); });
      } else if (prevTotal > 0 && total == 0) {
        LOG_NOTE("WinRT: emitCentralDisconnected (prevTotal=%d -> total=0)", prevTotal);
        emitCentralDisconnected();
        worker.post([this] { releasePreferredConnRequest(); });
      }
      // Pull the negotiated ATT MTU from the peer's GattSession now that a
      // subscriber exists. The peripheral does not initiate MTU exchange on
      // Windows; it waits for the central. This catches the post-exchange
      // state and propagates it up through mtuChanged.
      refreshNegotiatedMtuFromSubscribers();
    };
    tokSubscribedChangedStatus = pairingStatus.SubscribedClientsChanged(
        [this, updateSubscribers](wgap::GattLocalCharacteristic const &sender,
                                  wfnd::IInspectable const &) {
          updateSubscribers("PairingStatus", pairingStatusSubscribers, sender);
        });
    tokSubscribedChangedDownstream = dataDownstream.SubscribedClientsChanged(
        [this, updateSubscribers](wgap::GattLocalCharacteristic const &sender,
                                  wfnd::IInspectable const &) {
          updateSubscribers("DataDownstream", downstreamSubscribers, sender);
        });

    tokAdvertisementStatusChanged = serviceProvider.AdvertisementStatusChanged(
        [this](wgap::GattServiceProvider const &sender,
               wgap::GattServiceProviderAdvertisementStatusChangedEventArgs const &args) {
          auto status = sender.AdvertisementStatus();
          LOG_NOTE("WinRT: advertisement status -> %d error=%d",
                   static_cast<int>(status), static_cast<int>(args.Error()));
          if (status == wgap::GattServiceProviderAdvertisementStatus::Started) {
            if (!started.exchange(true))
              emitStarted();
          } else if (status == wgap::GattServiceProviderAdvertisementStatus::Aborted) {
            if (starting && !started) {
              LOG_WARN("WinRT: advertisement aborted during startup attempt; trying fallback if available");
              return;
            }
            emitStartFailed(QStringLiteral("advertisement aborted by stack, error=%1")
                                .arg(static_cast<int>(args.Error())));
          }
        });
  }

  void workerStop()
  {
    LOG_NOTE("WinRT: workerStop");
    releasePreferredConnRequest();
    stopManufacturerDataAdvertiser();
    try {
      if (serviceProvider) {
        if (tokAdvertisementStatusChanged.value)
          serviceProvider.AdvertisementStatusChanged(tokAdvertisementStatusChanged);
        if (serviceProvider.AdvertisementStatus() != wgap::GattServiceProviderAdvertisementStatus::Stopped)
          serviceProvider.StopAdvertising();
      }
      if (pairingAuth && tokPairingAuthWrite.value)
        pairingAuth.WriteRequested(tokPairingAuthWrite);
      if (dataUpstream && tokDataUpstreamWrite.value)
        dataUpstream.WriteRequested(tokDataUpstreamWrite);
      if (pairingStatus && tokSubscribedChangedStatus.value)
        pairingStatus.SubscribedClientsChanged(tokSubscribedChangedStatus);
      if (dataDownstream && tokSubscribedChangedDownstream.value)
        dataDownstream.SubscribedClientsChanged(tokSubscribedChangedDownstream);
    } catch (...) {
    }
    serviceProvider = nullptr;
    pairingAuth = nullptr;
    pairingStatus = nullptr;
    dataDownstream = nullptr;
    dataUpstream = nullptr;
    control = nullptr;
    started = false;
    starting = false;
    pairingStatusSubscribers = 0;
    downstreamSubscribers = 0;
    {
      std::lock_guard<std::mutex> lock(dsMu);
      dsQueue.clear();
      dsPumpScheduled = false;
    }
  }

  void workerNotify(wgap::GattLocalCharacteristic &ch, const QByteArray &chunk)
  {
    if (!ch) {
      LOG_WARN("WinRT: workerNotify dropped %d bytes (characteristic null)", chunk.size());
      return;
    }
    // MTU may finalise only after the first writes/notifies on some host
    // stacks, so probe again here. Cheap and idempotent.
    refreshNegotiatedMtuFromSubscribers();

    // Windows GATT server occasionally throws E_ILLEGAL_METHOD_CALL (0x8000000E)
    // on a NotifyValueAsync that immediately follows another notify, especially
    // across two different characteristics on the same connection. The link-
    // layer transmission is still settling even though the previous .get()
    // returned. Retry a few times with a brief backoff before giving up.
    constexpr int kMaxAttempts = 5;
    constexpr DWORD kBackoffMs = 15;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      try {
        auto buf = qByteArrayToIBuffer(chunk);
        ch.NotifyValueAsync(buf).get();
        LOG_DEBUG("WinRT: notify size=%d mtu=%d statusSubs=%d downSubs=%d attempt=%d",
                  chunk.size(), mtu.load(), pairingStatusSubscribers.load(),
                  downstreamSubscribers.load(), attempt);
        return;
      } catch (const winrt::hresult_error &e) {
        const auto hr = static_cast<unsigned>(e.code().value);
        const bool retryable = (hr == 0x8000000E /* E_ILLEGAL_METHOD_CALL */);
        if (retryable && attempt < kMaxAttempts) {
          LOG_DEBUG("WinRT: NotifyValueAsync hr=0x%08x size=%d attempt=%d, retrying",
                    hr, chunk.size(), attempt);
          ::Sleep(kBackoffMs * attempt);
          continue;
        }
        LOG_WARN("WinRT: NotifyValueAsync threw hr=0x%08x size=%d attempts=%d",
                 hr, chunk.size(), attempt);
        return;
      } catch (const std::exception &e) {
        LOG_WARN("WinRT: NotifyValueAsync threw std::exception: %s size=%d", e.what(), chunk.size());
        return;
      }
    }
  }

  // Pipelined fire-and-forget notify on dataDownstream. Mirrors the central
  // backend's WriteValueAsync pattern (WinRtBleCentralBackend.cpp). Increments
  // the shared in-flight counter on submit; the Completed callback decrements
  // it. The pump (workerPumpDownstream) gates submissions on this counter so
  // we never let more than kDownstreamInflightWindow ops sit in the OS stack.
  void workerSubmitDownstreamAsync(const QByteArray &chunk)
  {
    if (!dataDownstream) {
      LOG_WARN("WinRT: workerSubmitDownstreamAsync dropped %d bytes (characteristic null)",
               chunk.size());
      return;
    }
    refreshNegotiatedMtuFromSubscribers();
    std::shared_ptr<std::atomic<int>> inflight = dsInflight;
    int sz = chunk.size();
    try {
      auto buf = qByteArrayToIBuffer(chunk);
      auto op = dataDownstream.NotifyValueAsync(buf);
      inflight->fetch_add(1);
      // Completed runs on the Windows thread pool. Capture only the shared
      // atomic so it stays valid regardless of Impl lifetime.
      op.Completed(
          [inflight, sz](
              wfnd::IAsyncOperation<wfc::IVectorView<wgap::GattClientNotificationResult>> const &asyncOp,
              wfnd::AsyncStatus status) {
            inflight->fetch_sub(1);
            if (status != wfnd::AsyncStatus::Completed) {
              try {
                asyncOp.GetResults();
              } catch (const winrt::hresult_error &e) {
                LOG_DEBUG("WinRT: notify completion hr=0x%08x size=%d",
                          static_cast<unsigned>(e.code().value), sz);
              } catch (...) {
              }
            }
          });
    } catch (const winrt::hresult_error &e) {
      // Synchronous throw (E_ILLEGAL_METHOD_CALL etc.) — drop chunk; the central's
      // BleFramingReader handles gaps via packet-id resync.
      LOG_DEBUG("WinRT: NotifyValueAsync issue threw hr=0x%08x size=%d",
                static_cast<unsigned>(e.code().value), chunk.size());
    } catch (const std::exception &e) {
      LOG_WARN("WinRT: NotifyValueAsync issue threw std::exception: %s size=%d",
               e.what(), chunk.size());
    }
  }

  // Drain dsQueue, submitting fire-and-forget notifies up to the in-flight
  // window. When the window is full, briefly yield and re-check — the worker
  // thread is dedicated to BLE I/O so a sub-millisecond sleep does not starve
  // anything. Lifetime safe: only touches Impl members from the worker thread,
  // which is stopped before Impl is destroyed.
  void workerPumpDownstream()
  {
    while (true) {
      QByteArray next;
      {
        std::lock_guard<std::mutex> lock(dsMu);
        if (dsQueue.empty()) {
          dsPumpScheduled = false;
          return;
        }
        next = dsQueue.front();
        dsQueue.pop_front();
      }
      // Backpressure on the OS stack — wait for an in-flight op to complete
      // before we submit the next one.
      while (dsInflight->load() >= kDownstreamInflightWindow) {
        ::Sleep(1);
      }
      workerSubmitDownstreamAsync(next);
    }
  }

  size_t coalescePendingMouseMoveLocked()
  {
    size_t removed = 0;
    for (auto it = dsQueue.begin(); it != dsQueue.end();) {
      if (isMouseMoveBleChunk(*it)) {
        it = dsQueue.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    return removed;
  }

  bool dropOneQueuedChunkForOverflowLocked()
  {
    for (auto it = dsQueue.begin(); it != dsQueue.end(); ++it) {
      if (isMouseMoveBleChunk(*it)) {
        dsQueue.erase(it);
        return true;
      }
    }
    if (dsQueue.empty())
      return false;
    dsQueue.pop_front();
    return true;
  }
};

WinRtBlePeripheralBackend::WinRtBlePeripheralBackend(QObject *parent)
    : IBlePeripheralBackend(parent), m_impl(std::make_unique<Impl>())
{
  m_impl->owner = this;
  m_impl->worker.start();
}

WinRtBlePeripheralBackend::~WinRtBlePeripheralBackend()
{
  if (m_impl) {
    // Ensure any pending WinRT work completes before we tear down.
    m_impl->worker.post([this] { m_impl->workerStop(); });
    m_impl->worker.stop();
  }
}

bool WinRtBlePeripheralBackend::start(const QString &localName, const QByteArray &manufacturerPayload)
{
  LOG_NOTE("WinRtBlePeripheralBackend::start (dispatching to worker)");
  m_impl->localName = localName;
  m_impl->mfgPayload = manufacturerPayload;
  m_impl->worker.post([this] {
    try {
      m_impl->workerStart();
    } catch (const winrt::hresult_error &e) {
      LOG_ERR("WinRT: workerStart escaped hr=0x%08x msg=%ls",
              static_cast<unsigned>(e.code().value), e.message().c_str());
      if (m_impl->started) {
        LOG_WARN("WinRT: ignoring late workerStart hresult after advertising started");
        return;
      }
      m_impl->emitStartFailed(QStringLiteral("WinRT workerStart hr=0x%1")
                                  .arg(QString::number(static_cast<quint32>(e.code().value), 16)));
    } catch (const std::exception &e) {
      LOG_ERR("WinRT: workerStart escaped std::exception: %s", e.what());
      if (m_impl->started) {
        LOG_WARN("WinRT: ignoring late workerStart exception after advertising started");
        return;
      }
      m_impl->emitStartFailed(QStringLiteral("WinRT workerStart exception: %1")
                                  .arg(QString::fromUtf8(e.what())));
    } catch (...) {
      LOG_ERR("WinRT: workerStart escaped unknown exception");
      if (m_impl->started) {
        LOG_WARN("WinRT: ignoring late workerStart unknown exception after advertising started");
        return;
      }
      m_impl->emitStartFailed(QStringLiteral("WinRT workerStart unknown exception"));
    }
  });
  return true; // async result via `started` or `startFailed` signals
}

void WinRtBlePeripheralBackend::stop()
{
  if (m_impl)
    m_impl->worker.post([this] { m_impl->workerStop(); });
}

void WinRtBlePeripheralBackend::sendPairingStatus(quint8 status)
{
  if (!m_impl)
    return;
  QByteArray payload(1, static_cast<char>(status));
  m_impl->worker.post([this, payload] {
    m_impl->workerNotify(m_impl->pairingStatus, payload);
  });
}

void WinRtBlePeripheralBackend::sendDownstream(const QByteArray &chunk)
{
  if (!m_impl)
    return;
  bool needPump = false;
  size_t dropped = 0;
  size_t coalescedMouse = 0;
  size_t depthAfter = 0;
  {
    std::lock_guard<std::mutex> lock(m_impl->dsMu);
    if (isMouseMoveBleChunk(chunk))
      coalescedMouse = m_impl->coalescePendingMouseMoveLocked();
    m_impl->dsQueue.push_back(chunk);
    while (m_impl->dsQueue.size() > Impl::kDownstreamQueueCap) {
      if (!m_impl->dropOneQueuedChunkForOverflowLocked())
        break;
      ++dropped;
    }
    depthAfter = m_impl->dsQueue.size();
    if (!m_impl->dsPumpScheduled) {
      m_impl->dsPumpScheduled = true;
      needPump = true;
    }
  }
  if (coalescedMouse) {
    const auto total = m_impl->dsCoalescedMouseTotal.fetch_add(coalescedMouse) + coalescedMouse;
    LOG_DEBUG("WinRT: downstream coalesced %zu pending mouse chunk(s), depth=%zu lifetimeTotal=%llu",
              coalescedMouse, depthAfter, static_cast<unsigned long long>(total));
  }
  if (dropped) {
    const auto total = m_impl->dsDroppedTotal.fetch_add(dropped) + dropped;
    LOG_WARN("WinRT: downstream queue overflow — dropped %zu old chunk(s), total dropped=%llu",
             dropped, static_cast<unsigned long long>(total));
  }
  if (needPump) {
    auto *impl = m_impl.get();
    m_impl->worker.post([impl] { impl->workerPumpDownstream(); });
  }
}

int WinRtBlePeripheralBackend::negotiatedMtu() const
{
  return m_impl ? m_impl->mtu.load() : 64;
}

} // namespace deskflow::ble
