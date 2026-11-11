# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Project tuning lives in sdkconfig.local, layered over sdkconfig.defaults.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Firmware version. Stamped into the app descriptor (esp_app_get_description())
# and shown on the web UI. Bump on every released build. The OTA validator
# rejects an uploaded image whose project name differs from this one.
set(PROJECT_VER "0.1.0")

project(vseismo)

# SNTP sample quality. Stock lwIP takes the server's transmit time as is, so
# reply delay on a jittery uplink becomes clock error. Compensate for the
# round trip (RFC 4330 offset) and accept only replies that echo our own
# request timestamp, from the server we asked.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${lwip_lib} PRIVATE SNTP_COMP_ROUNDTRIP=1 SNTP_CHECK_RESPONSE=2)
