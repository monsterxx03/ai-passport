cmake_minimum_required(VERSION 3.16)

# 一个独立的小 app：BLE 配对探针（不是产品固件）。它不复用 components/bsp——
# 探针不需要屏幕，只要 NimBLE 和串口日志，这样它跟产品固件的依赖完全隔离。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ble-probe)
