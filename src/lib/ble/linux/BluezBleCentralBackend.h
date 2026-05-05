/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "ble/IBleCentralBackend.h"

#include <QByteArray>
#include <QDBusObjectPath>
#include <QHash>
#include <QString>
#include <QStringList>

class QDBusPendingCallWatcher;

namespace deskflow::ble::bluez {
class BluezAdapter;
}

namespace deskflow::ble {

// Linux central backend speaking org.bluez directly. Mirrors WinRtBleCentral
// Backend: scans for the Deskflow service via ManufacturerData magic, connects,
// resolves the GATT service, writes the pairing code, subscribes to downstream
// notifies, and exposes a fire-and-forget (or lossless) upstream write path.
class BluezBleCentralBackend : public IBleCentralBackend
{
  Q_OBJECT
public:
  explicit BluezBleCentralBackend(QObject *parent = nullptr);
  ~BluezBleCentralBackend() override;

  void start(const QString &savedDeviceId, const QString &code, quint64 directAddress = 0) override;
  void stop() override;
  void writeUpstream(const QByteArray &chunk) override;
  void setUpstreamLossless(bool lossless) override;
  int mtu() const override
  {
    return m_mtu;
  }
  quint64 peerAddress() const override
  {
    return m_peerAddress;
  }

  // Public so tests can exercise the path construction without DBus.
  static QString deviceObjectPathFromAddress(const QString &adapterPath, quint64 addr);

private Q_SLOTS:
  // The wire type for InterfacesAdded's second argument is a{sa{sv}}, which
  // Qt represents as QMap<QString, QVariantMap>. Using QVariantMap here
  // would silently not match the signal at dispatch time.
  void onInterfacesAdded(const QDBusObjectPath &path,
                         const QMap<QString, QVariantMap> &interfaces);
  void onDevicePropertiesChanged(const QString &interface, const QVariantMap &changed,
                                 const QStringList &invalidated);
  void onPairingStatusPropertiesChanged(const QString &interface, const QVariantMap &changed,
                                        const QStringList &invalidated);
  void onDownstreamPropertiesChanged(const QString &interface, const QVariantMap &changed,
                                     const QStringList &invalidated);

private:
  enum class State
  {
    Idle,
    Scanning,
    Connecting,
    Resolving,
    Discovering,
    AwaitingPair,
    Ready,
    Failed,
  };

  void startScan();
  void stopScan();
  bool considerDevice(const QString &path, const QVariantMap &deviceProps);
  void connectDevice(const QString &devicePath);
  void onConnectFinished(QDBusPendingCallWatcher *watcher);
  void onServicesResolved();
  void discoverCharacteristics();
  void writePairingCode();
  void emitFail(const QString &reason);
  void teardownInternal();

  bluez::BluezAdapter *m_adapter = nullptr;

  // Configuration captured from start().
  QString m_savedDeviceId;
  QString m_pendingCode;
  QByteArray m_expectedHash;
  quint64 m_directAddress = 0;

  // Live state.
  State m_state = State::Idle;
  QString m_devicePath;
  quint64 m_peerAddress = 0;
  int m_mtu = 64;
  bool m_upstreamLossless = true;
  bool m_pairingAccepted = false;

  // Cached GATT char paths.
  QString m_charPairingAuth;
  QString m_charPairingStatus;
  QString m_charDataDownstream;
  QString m_charDataUpstream;
  QString m_charControl;
};

} // namespace deskflow::ble
