# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

create_test(
  NAME BlePairingCodeTests
  DEPENDS ble
  LIBS base common
  SOURCE BlePairingCodeTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/ble"
)

create_test(
  NAME BleFramingTests
  DEPENDS ble
  LIBS base common
  SOURCE BleFramingTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/ble"
)

create_test(
  NAME BleProtocolClassifierTests
  DEPENDS ble
  LIBS base common
  SOURCE BleProtocolClassifierTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/ble"
)

create_test(
  NAME BleAddressParserTests
  DEPENDS ble
  LIBS base common
  SOURCE BleAddressParserTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/ble"
)

if(UNIX AND NOT APPLE)
  create_test(
    NAME BluezHelpersTests
    DEPENDS ble
    LIBS base common
    SOURCE BluezHelpersTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/ble"
  )
endif()

if(WIN32)
  # OpenSSL::applink must be linked directly to the test executable on
  # Windows: applink.c contains the OpenSSL/MSVC CRT bridge thunks and
  # they only work when compiled into the same module as the OpenSSL
  # callers (INTERFACE_SOURCES don't propagate transitively through
  # static libs). version is needed for arch's ArchMiscWindows.
  find_package(OpenSSL ${REQUIRED_OPENSSL_VERSION} REQUIRED COMPONENTS SSL Crypto)
  set(ble_secure_extra_libs version OpenSSL::applink)
endif()

create_test(
  NAME BleSecureSocketTests
  DEPENDS ble
  LIBS base common net arch mt io ${ble_secure_extra_libs}
  SOURCE BleSecureSocketTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/ble"
)
