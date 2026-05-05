/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2016 - 2025 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QSettings>

#include <QDir>

#include "common/Constants.h"
#include "common/QSettingsProxy.h"

class Settings : public QObject
{
  Q_OBJECT
public:
#if defined(Q_OS_WIN)
  inline const static auto UserDir = QStringLiteral("%1/AppData/Roaming/%2").arg(QDir::homePath(), kAppName);
  inline const static auto SystemDir = QStringLiteral("%1ProgramData/%2").arg(QDir::rootPath(), kAppName);
#elif defined(Q_OS_MACOS)
  inline const static auto UserDir = QStringLiteral("%1/Library/%2").arg(QDir::homePath(), kAppName);
  inline const static auto SystemDir = QStringLiteral("/Library/%1").arg(kAppName);
#else
  inline const static auto UserDir = QStringLiteral("%1/.config/%2").arg(QDir::homePath(), kAppName);
  inline const static auto SystemDir = QStringLiteral("/etc/%1").arg(kAppName);
#endif

  inline const static auto UserSettingFile = QStringLiteral("%1/%2.conf").arg(UserDir, kAppName);
  inline const static auto SystemSettingFile = QStringLiteral("%1/%2.conf").arg(SystemDir, kAppName);

  struct Client
  {
    inline static const auto DynamicConnectionRetry = QStringLiteral("client/dynamicConnectionInterval");
    inline static const auto InvertYScroll = QStringLiteral("client/invertYScroll");
    inline static const auto InvertXScroll = QStringLiteral("client/invertXScroll");
    inline static const auto YScrollScale = QStringLiteral("client/yScrollScale");
    inline static const auto XScrollScale = QStringLiteral("client/xScrollScale");
    inline static const auto LanguageSync = QStringLiteral("client/languageSync");
    inline static const auto RemoteHost = QStringLiteral("client/remoteHost");
    inline static const auto RemoteBleDevice = QStringLiteral("client/remoteBleDevice");
    // Ephemeral: GUI writes the 6-digit code here before starting core;
    // core reads and clears it during BLE connect.
    inline static const auto PendingBleCode = QStringLiteral("client/pendingBleCode");
    // Optional: explicit 48-bit BT address (12 hex chars, e.g. "047F0E728E39"
    // or "04:7F:0E:72:8E:39") for adapters whose peripheral-mode advertising
    // can't be discovered via scan. When set, the central skips scanning and
    // calls FromBluetoothAddressAsync directly.
    inline static const auto DirectBleAddress = QStringLiteral("client/directBleAddress");
    inline static const auto XdpRestoreToken = QStringLiteral("client/xdpRestoreToken");
  };
  struct Core
  {
    inline static const auto CoreMode = QStringLiteral("core/coreMode");
    inline static const auto Interface = QStringLiteral("core/interface");
    inline static const auto LastVersion = QStringLiteral("core/lastVersion");
    inline static const auto Port = QStringLiteral("core/port");
    inline static const auto Transport = QStringLiteral("core/transport"); // "tcp" | "ble"
    inline static const auto BleBackend = QStringLiteral("core/bleBackend"); // "winrt" | "qt"
    // BLE transport reliability. Default true. Each host applies this key to
    // the direction it *sends* — when running as server (peripheral) it gates
    // GATT notify between sync .get() (lossless) and fire-and-forget (lossy);
    // when running as client (central) it gates GATT write between
    // WriteWithResponse (lossless) and WriteWithoutResponse (lossy). Lossless
    // avoids the BleFraming drop-and-resync that breaks TLS record alignment.
    inline static const auto BleStreamLossless = QStringLiteral("core/bleStreamLossless");
    inline static const auto PreventSleep = QStringLiteral("core/preventSleep");
    inline static const auto ProcessMode = QStringLiteral("core/processMode");
    inline static const auto ComputerName = QStringLiteral("core/computerName");
    inline static const auto Display = QStringLiteral("core/display");
    inline static const auto UseHooks = QStringLiteral("core/useHooks");
    inline static const auto Language = QStringLiteral("core/language");
    inline static const auto UseWlClipboard = QStringLiteral("core/wlClipboard");
    inline static const auto EnableEnterCommand = QStringLiteral("core/enableEnterCommand");
    inline static const auto ScreenEnterCommand = QStringLiteral("core/enterCommand");
    inline static const auto EnableExitCommand = QStringLiteral("core/enableExitCommand");
    inline static const auto ScreenExitCommand = QStringLiteral("core/exitCommand");

    // TODO: REMOVE In 2.0
    inline static const auto ScreenName = QStringLiteral("core/screenName"); // Replaced By ComputerName
  };
  struct Daemon
  {
    inline static const auto ConfigFile = QStringLiteral("daemon/configFile");
    inline static const auto Elevate = QStringLiteral("daemon/elevate");
    inline static const auto LogFile = QStringLiteral("daemon/logFile");
    inline static const auto LogLevel = QStringLiteral("daemon/logLevel");
  };
  struct Gui
  {
    inline static const auto Autohide = QStringLiteral("gui/autoHide");
    inline static const auto AutoStartCore = QStringLiteral("gui/startCoreWithGui");
    inline static const auto AutoUpdateCheck = QStringLiteral("gui/enableUpdateCheck");
    inline static const auto UpdateCheckUrl = QStringLiteral("gui/updateCheckUrl");
    inline static const auto CloseReminder = QStringLiteral("gui/closeReminder");
    inline static const auto CloseToTray = QStringLiteral("gui/closeToTray");
    inline static const auto LogExpanded = QStringLiteral("gui/logExpanded");
    inline static const auto SymbolicTrayIcon = QStringLiteral("gui/symbolicTrayIcon");
    inline static const auto WindowGeometry = QStringLiteral("gui/windowGeometry");
    inline static const auto ShownFirstConnectedMessage = QStringLiteral("gui/shownFirstConnectedMessage");
    inline static const auto ShownServerFirstStartMessage = QStringLiteral("gui/shownServerFirstStartMessage");
    inline static const auto ShowVersionInTitle = QStringLiteral("gui/showVersionInTitle");
    inline static const auto IgnoreMissingKeyboardLayouts = QStringLiteral("gui/ignoreMissingKeyboardLayouts");
  };
  struct Log
  {
    inline static const auto File = QStringLiteral("log/file");
    inline static const auto Level = QStringLiteral("log/level");
    inline static const auto ToFile = QStringLiteral("log/toFile");
    inline static const auto GuiDebug = QStringLiteral("log/guiDebug");
  };
  struct Security
  {
    inline static const auto CheckPeers = QStringLiteral("security/checkPeerFingerprints");
    inline static const auto Certificate = QStringLiteral("security/certificate");
    inline static const auto KeySize = QStringLiteral("security/keySize");
    inline static const auto TlsEnabled = QStringLiteral("security/tlsEnabled");
  };
  struct Server
  {
    inline static const auto ExternalConfig = QStringLiteral("server/externalConfig");
    inline static const auto ExternalConfigFile = QStringLiteral("server/externalConfigFile");
    inline static const auto Protocol = QStringLiteral("server/protocol");
    // Set true after the first successful BLE pairing on this host. While set,
    // the BLE peripheral context auto-accepts a remembered-peer reconnect
    // (i.e. a central that subscribes without writing the per-session code).
    inline static const auto HasBlePairedPeer = QStringLiteral("server/hasBlePairedPeer");
    // Persisted 6-digit BLE pairing PIN. Generated lazily on first server
    // start, reused on subsequent starts so a client config with a fixed
    // pendingBleCode keeps working across server restarts. Cleared by the
    // --regen-ble-code CLI flag to force a fresh PIN.
    inline static const auto BlePairingCode = QStringLiteral("server/blePairingCode");
  };

  // Enums types used in settings
  // The use of enum classes is not use for these
  // enum classes are more specific when used with QVariant
  // This leads longer function calls in code
  // and longer more cryptic output in the settings file
  // The using of standard enum will just write ints
  // and we can read / write them as if they were ints
  enum ProcessMode
  {
    Service,
    Desktop
  };
  Q_ENUM(ProcessMode)

  enum CoreMode
  {
    None,
    Client,
    Server
  };
  Q_ENUM(CoreMode)

  static Settings *instance();
  static void setSettingsFile(const QString &settingsFile = QString());
  static void setStateFile(const QString &stateFile = QString());
  static void setValue(const QString &key = QString(), const QVariant &value = QVariant());
  static QVariant value(const QString &key = QString());
  static void restoreDefaultSettings();
  static QVariant defaultValue(const QString &key);
  static bool isServerConfigFileReadable();
  static bool isWritable();
  static bool isPortableMode();
  static QString settingsFile();
  static QString settingsPath();
  static QString serverConfigFile();
  static QString tlsDir();
  static QString tlsTrustedServersDb();
  static QString tlsTrustedClientsDb();
  static QString logLevelText();
  static QSettingsProxy &proxy();
  static void save(bool emitSaving = true);
  static QStringList validKeys();
  static int logLevelToInt(const QString &level);
  static QString portableSettingsFile();

Q_SIGNALS:
  void settingsChanged(const QString key);
  void serverSettingsChanged();

private:
  explicit Settings(QObject *parent = nullptr);
  Settings *operator=(Settings &other) = delete;
  Settings(const Settings &other) = delete;
  ~Settings() override = default;

  /**
   * @brief This method uses the Settings::m_upgradeMap, keys are upgraded if the oldkey is found and the newKey is not
   * This method is run when settings is created before cleaning the settings and when you change settings files
   * It does not remove any keys, only copies the old value to the new setings key
   */
  void upgradeSettings();
  void cleanSettings();
  void cleanStateSettings();

  /**
   * @brief write an initial computer name
   */
  void setupComputerName();

  /**
   * @brief cleanComputerName ensure a valid computerName from the provided one
   * @param name any string to be used as the computerName
   * @return a valid computerName
   */
  static QString cleanComputerName(const QString &name);

  QSettings *m_settings = nullptr;
  QSettings *m_stateSettings = nullptr;
  std::shared_ptr<QSettingsProxy> m_settingsProxy;

  // clang-format off
  inline static const QStringList m_logLevels = {
      QStringLiteral("FATAL")
    , QStringLiteral("ERROR")
    , QStringLiteral("WARNING")
    , QStringLiteral("NOTE")
    , QStringLiteral("INFO")
    , QStringLiteral("DEBUG")
    , QStringLiteral("DEBUG1")
    , QStringLiteral("DEBUG2")
  };

  inline static const QStringList m_validKeys = {
      Settings::Client::DynamicConnectionRetry
    , Settings::Client::InvertYScroll
    , Settings::Client::InvertXScroll
    , Settings::Client::LanguageSync
    , Settings::Client::RemoteHost
    , Settings::Client::YScrollScale
    , Settings::Client::XScrollScale
    , Settings::Client::XdpRestoreToken
    , Settings::Client::RemoteBleDevice
    , Settings::Client::PendingBleCode
    , Settings::Client::DirectBleAddress
    , Settings::Core::CoreMode
    , Settings::Core::Interface
    , Settings::Core::LastVersion
    , Settings::Core::Port
    , Settings::Core::Transport
    , Settings::Core::BleBackend
    , Settings::Core::BleStreamLossless
    , Settings::Core::PreventSleep
    , Settings::Core::ProcessMode
    , Settings::Core::EnableEnterCommand
    , Settings::Core::EnableExitCommand
    , Settings::Core::ScreenEnterCommand
    , Settings::Core::ScreenExitCommand
    , Settings::Core::ScreenName
    , Settings::Core::ComputerName
    , Settings::Core::Display
    , Settings::Core::UseHooks
    , Settings::Core::UseWlClipboard
    , Settings::Core::Language
    , Settings::Daemon::ConfigFile
    , Settings::Daemon::Elevate
    , Settings::Daemon::LogFile
    , Settings::Daemon::LogLevel
    , Settings::Log::File
    , Settings::Log::Level
    , Settings::Log::ToFile
    , Settings::Log::GuiDebug
    , Settings::Gui::Autohide
    , Settings::Gui::AutoStartCore
    , Settings::Gui::AutoUpdateCheck
    , Settings::Gui::UpdateCheckUrl
    , Settings::Gui::CloseReminder
    , Settings::Gui::CloseToTray
    , Settings::Gui::LogExpanded
    , Settings::Gui::SymbolicTrayIcon
    , Settings::Gui::WindowGeometry
    , Settings::Gui::ShownFirstConnectedMessage
    , Settings::Gui::ShownServerFirstStartMessage
    , Settings::Gui::ShowVersionInTitle
    , Settings::Gui::IgnoreMissingKeyboardLayouts
    , Settings::Security::Certificate
    , Settings::Security::CheckPeers
    , Settings::Security::KeySize
    , Settings::Security::TlsEnabled
    , Settings::Server::ExternalConfig
    , Settings::Server::ExternalConfigFile
    , Settings::Server::Protocol
    , Settings::Server::HasBlePairedPeer
    , Settings::Server::BlePairingCode
  };

  // When checking the default values this list contains the ones that default to false.
  inline static const QStringList m_defaultFalseValues = {
      Settings::Gui::Autohide
    , Settings::Gui::AutoStartCore
    , Settings::Gui::ShownFirstConnectedMessage
    , Settings::Gui::ShownServerFirstStartMessage
    , Settings::Gui::ShowVersionInTitle
    , Settings::Gui::IgnoreMissingKeyboardLayouts
    , Settings::Core::PreventSleep
    , Settings::Core::UseWlClipboard
    , Settings::Core::EnableEnterCommand
    , Settings::Core::EnableExitCommand
    , Settings::Client::DynamicConnectionRetry
    , Settings::Server::ExternalConfig
    , Settings::Client::InvertYScroll
    , Settings::Client::InvertXScroll
    , Settings::Log::ToFile
    , Settings::Log::GuiDebug
  };

  // When checking the default values this list contains the ones that default to true.
  inline static const QStringList m_defaultTrueValues = {
      Settings::Core::UseHooks
    , Settings::Client::LanguageSync
    , Settings::Gui::CloseToTray
    , Settings::Gui::CloseReminder
    , Settings::Gui::LogExpanded
    , Settings::Gui::SymbolicTrayIcon
    , Settings::Security::TlsEnabled
    , Settings::Security::CheckPeers
    , Settings::Core::BleStreamLossless
  };

  // Settings saved in our State file
  inline static const QStringList m_stateKeys = { Settings::Gui::WindowGeometry };

  // Contains settings keys to be upgraded.
  inline static const QMap<QString, QString> m_upgradedMap = {
    /*             OLD KEY                        NEW KEY          */
    {QStringLiteral("core/screenName"), Settings::Core::ComputerName}
  };
  // clang-format on
};
