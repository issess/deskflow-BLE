# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

find_package(Qt6 ${REQUIRED_QT_VERSION} REQUIRED COMPONENTS Bluetooth)

set(ble_sources
  BleAddressParser.h
  BleFraming.cpp
  BleFraming.h
  BleListenSocket.cpp
  BleListenSocket.h
  BlePairingBroker.cpp
  BlePairingBroker.h
  BlePairingCode.cpp
  BlePairingCode.h
  BleProtocolClassifier.cpp
  BleProtocolClassifier.h
  BleSecureListenSocket.cpp
  BleSecureListenSocket.h
  BleSecureSocket.cpp
  BleSecureSocket.h
  BleSocket.cpp
  BleSocket.h
  BleSocketFactory.cpp
  BleSocketFactory.h
  BleTransport.h
  IBleCentralBackend.h
  IBlePeripheralBackend.h
  qt/QtBlePeripheralBackend.cpp
  qt/QtBlePeripheralBackend.h
)

if(WIN32)
  list(APPEND ble_sources
    win/WinRtBlePeripheralBackend.cpp
    win/WinRtBlePeripheralBackend.h
    win/WinRtBleCentralBackend.cpp
    win/WinRtBleCentralBackend.h
  )
endif()

if(UNIX AND NOT APPLE)
  list(APPEND ble_sources
    linux/BluezAdapter.cpp
    linux/BluezAdapter.h
    linux/BluezAdvertisement.cpp
    linux/BluezAdvertisement.h
    linux/BluezBleCentralBackend.cpp
    linux/BluezBleCentralBackend.h
    linux/BluezBlePeripheralBackend.cpp
    linux/BluezBlePeripheralBackend.h
    linux/BluezDbusTypes.cpp
    linux/BluezDbusTypes.h
    linux/BluezGattApplication.cpp
    linux/BluezGattApplication.h
  )
endif()

add_library(ble STATIC ${ble_sources})

set_target_properties(ble PROPERTIES AUTOMOC ON)

target_link_libraries(ble
  PUBLIC Qt6::Bluetooth common net
  PRIVATE base io
)

if(WIN32)
  # C++/WinRT support — enable coroutines via C++20 (already set project-wide)
  # and link the runtime object library which provides WinRT activation.
  target_link_libraries(ble PRIVATE WindowsApp runtimeobject)
  # The Windows 10 SDK ships generated C++/WinRT headers under
  # <SDK>/Include/<version>/cppwinrt/winrt. MSVC normally finds them via
  # default include paths; if a specific SDK version is selected, CMake's
  # default toolchain integration exposes them. No explicit include dir
  # typically needed.
  target_compile_definitions(ble PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
  )
endif()

if(UNIX AND NOT APPLE)
  # Linux BlueZ backend speaks org.bluez over the system bus via Qt6::DBus.
  # Already required at the project level (cmake/Libraries.cmake) but the ble
  # target needs an explicit link so AUTOMOC sees the metatype registration.
  find_package(Qt6 ${REQUIRED_QT_VERSION} REQUIRED COMPONENTS DBus)
  target_link_libraries(ble PUBLIC Qt6::DBus)
endif()
