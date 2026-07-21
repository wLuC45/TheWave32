cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../tw32_common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_deauth)

# Bypass the closed-source net80211 raw-frame sanity check (deauth
# spoofs the AP's BSSID).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/net80211_patch.cmake)
tw32_apply_net80211_patch()
