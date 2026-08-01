cmake_minimum_required(VERSION 3.16)

project(VDIClient VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

set(PROJECT_SOURCES
    src/main.cpp
    src/loginwindow.cpp
    src/loginwindow.h
    resources/resources.qrc
    resources/app.rc
)

add_executable(${PROJECT_NAME} WIN32 ${PROJECT_SOURCES})

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
    
    if(EXISTS "${CMAKE_SOURCE_DIR}/bin/qf-client.exe")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        add_custom_target(copy_qf_client
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/bin" "${CMAKE_BINARY_DIR}/bin"
            COMMENT "Copying bin directory (qf-client) to build directory"
        )
        add_dependencies(${PROJECT_NAME} copy_qf_client)
    endif()
    
    if(EXISTS "${CMAKE_SOURCE_DIR}/drivers/UsbDk_1.0.22_x64.msi")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/drivers")
        add_custom_target(copy_usbdk
            COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/drivers/UsbDk_1.0.22_x64.msi" "${CMAKE_BINARY_DIR}/drivers/"
            COMMENT "Copying UsbDk driver MSI to build directory"
        )
        add_dependencies(${PROJECT_NAME} copy_usbdk)
    endif()
endif()
