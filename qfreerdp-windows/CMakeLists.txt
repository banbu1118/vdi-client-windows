cmake_minimum_required(VERSION 3.16)
project(qf-client LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CLIPRDR_REF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/ref-tmp")
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 自动处理 Qt 的元对象和资源系统
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 使用自编译的 FreeRDP 3.28.0
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../freerdp-3.28.0/install")

# 1. 寻找 Qt 6 的核心与 QML 相关模块
# GuiPrivate 提供 QRhi 头文件 (QtGui/rhi/qrhi.h)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick GuiPrivate)
find_package(FreeRDP 3 REQUIRED)
find_package(WinPR 3 REQUIRED)
find_package(FreeRDP-Client 3 REQUIRED)
find_package(spdlog REQUIRED)
message(STATUS "FreeRDP_VERSION: ${FreeRDP_VERSION}")

# 2. 定义可执行目标
# 将 .rc 直接加入源列表，由 CMake 调用 rc.exe 编译并链接进 exe（内嵌程序图标）
qt_add_executable(qf-client
    src/mini-qf-client.cc
    src/usb-manager.cc
    src/qf_channel_client_handler.c
    ${CLIPRDR_REF_DIR}/cliprdr_main.c
    ${CLIPRDR_REF_DIR}/cliprdr_format.c
    ${CLIPRDR_REF_DIR}/cliprdr_common.c
    src/resources.qrc
    src/resources.rc
)

# 3. 注册 QML 模块
qt_add_qml_module(qf-client
    URI MyTestApp
    VERSION 1.0
    QML_FILES src/main.qml
    SOURCES src/rdp-view-item.h
)

# FreeRDP 3 installs headers under include/freerdp3.
target_include_directories(qf-client PRIVATE
    ${FreeRDP_INCLUDE_DIR}
    ${FreeRDP-Client_INCLUDE_DIR}
    ${WinPR_INCLUDE_DIR}
    ${CLIPRDR_REF_DIR}
    src
)

# libusb for USB device enumeration
find_path(LIBUSB_INCLUDE_DIR libusb-1.0/libusb.h)
find_library(LIBUSB_LIBRARY libusb-1.0)

if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
    target_include_directories(qf-client PRIVATE ${LIBUSB_INCLUDE_DIR})
    target_link_libraries(qf-client PRIVATE ${LIBUSB_LIBRARY})
    target_compile_definitions(qf-client PRIVATE WITH_LIBUSB=1)
    message(STATUS "libusb found: ${LIBUSB_LIBRARY}")
else()
    message(WARNING "libusb-1.0 not found — USB device enumeration disabled")
endif()

# MSVC compatibility: ref-tmp uses GCC __attribute__ extensions
if(MSVC)
    target_compile_options(qf-client PRIVATE "/wd4251" "/wd4275")
    # channel_client_create_handler is also in freerdp-client3.dll import lib;
    # our local copy is identical, allow duplicate symbol resolution
    target_link_options(qf-client PRIVATE "/FORCE:MULTIPLE")
endif()

# 链接 Qt 模块
target_link_libraries(qf-client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Qml
    Qt6::Quick
    freerdp
    freerdp-client
    winpr
    spdlog::spdlog
    winmm
    setupapi
)
