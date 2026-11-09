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
