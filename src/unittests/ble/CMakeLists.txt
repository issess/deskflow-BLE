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
