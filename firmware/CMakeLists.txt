# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 固件版本（esp_app_get_description()->version 的来源；与
# components/ota_shared/include/ota_version.h 的 OTA_APP_VERSION 保持一致）
set(PROJECT_VER "1.0.1")

# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

project(esp32c6_ble_ota)
