cmake_minimum_required(VERSION 3.16)

# 开机选择菜单：复用 esp-sdr tft 分支的屏幕/按键驱动，位于 ../common/panel。
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../common/panel"
    "${CMAKE_CURRENT_LIST_DIR}/../common/dualboot")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dual_boot_launcher)
