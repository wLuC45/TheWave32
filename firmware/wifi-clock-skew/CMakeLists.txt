cmake_minimum_required(VERSION 3.16)

# Pull in the shared firmware/tw32_common/ component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../tw32_common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_clock_skew)
