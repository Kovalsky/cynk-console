cmake_minimum_required(VERSION 3.16)
project(cynk_console LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CYNK_DEVICE_SDK_DIR "" CACHE PATH "Path to cynk-sdk-c")
if(NOT CYNK_DEVICE_SDK_DIR)
  if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/cynk-sdk-c/CMakeLists.txt")
    set(CYNK_DEVICE_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/cynk-sdk-c")
  else()
    set(CYNK_DEVICE_SDK_DIR "${CMAKE_SOURCE_DIR}/../cynk-sdk-c")
  endif()
endif()

if(NOT EXISTS "${CYNK_DEVICE_SDK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "cynk-sdk-c not found. Set CYNK_DEVICE_SDK_DIR to its path.")
endif()

set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/mbedtls)
add_subdirectory(${CYNK_DEVICE_SDK_DIR} cynk_device_sdk)

# ---------- Platform-specific compat source ----------
if(WIN32)
  set(COMPAT_SRC src/compat_win32.c)
else()
  set(COMPAT_SRC src/compat_posix.c)
endif()

add_library(mqttc STATIC
  third_party/mqtt-c/src/mqtt.c
  src/mqtt_pal.c
)

target_include_directories(mqttc
  PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/mqtt-c/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/mbedtls/include
)

target_compile_definitions(mqttc PUBLIC MQTTC_PAL_FILE=cynk_mqtt_pal.h)

target_link_libraries(mqttc
  PUBLIC
    mbedtls
    mbedx509
    mbedcrypto
)

add_executable(cynk-console
  src/main.c
  src/cli.c
  src/net.c
  ${COMPAT_SRC}
)

target_include_directories(cynk-console
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(cynk-console
  PRIVATE
    cynk_device
    mqttc
    mbedtls
    mbedx509
    mbedcrypto
)

# ---------- Platform-specific link dependencies ----------
if(WIN32)
  target_link_libraries(cynk-console PRIVATE ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(cynk-console PRIVATE Threads::Threads)
endif()
