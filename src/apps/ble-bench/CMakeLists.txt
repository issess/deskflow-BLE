# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

set(target ble-bench)

add_executable(${target}
  main.cpp
)

target_link_libraries(${target}
  PRIVATE
    arch
    base
    common
    io
    mt
    net
    ble
    Qt6::Core
    ${libs}
)
