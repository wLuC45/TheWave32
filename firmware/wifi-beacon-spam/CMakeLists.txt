cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../tw32_common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_beacon_spam)

# Beacon flooder uses random per-SSID BSSIDs; needs the same
# net80211 sanity-check bypass that wifi-deauth uses.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/net80211_patch.cmake)
tw32_apply_net80211_patch()
