cmake_minimum_required(VERSION 3.16)
project(gdesk VERSION 2.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network DBus WebEngineCore WebEngineWidgets)
find_package(Qt6Keychain REQUIRED)

add_executable(gdesk
    src/main.cpp
    src/googleauth.cpp src/googleauth.h
    src/gmailapi.cpp src/gmailapi.h
    src/mime.cpp src/mime.h
    src/mainwindow.cpp src/mainwindow.h
    src/messageview.cpp src/messageview.h
    src/composer.cpp src/composer.h
    src/setupdialog.cpp src/setupdialog.h
    src/settingsdialog.cpp src/settingsdialog.h
    src/theme.cpp src/theme.h
    data/gdesk.qrc)
target_compile_definitions(gdesk PRIVATE GDESK_VERSION="${PROJECT_VERSION}")
target_link_libraries(gdesk PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus
                                    Qt6::WebEngineCore Qt6::WebEngineWidgets Qt6Keychain::Qt6Keychain)

# Icônes PNG pour les menus et panneaux qui n'utilisent pas le SVG
set(GDESK_ICON_SIZES 16 22 24 32 48 64 128 256 512)
add_executable(gdesk-rasterize-icon tools/rasterize-icon.cpp)
target_link_libraries(gdesk-rasterize-icon PRIVATE Qt6::Gui)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/icons/512x512/apps/gdesk.png
    COMMAND gdesk-rasterize-icon ${CMAKE_SOURCE_DIR}/data/gdesk.svg ${CMAKE_BINARY_DIR}/icons ${GDESK_ICON_SIZES}
    DEPENDS gdesk-rasterize-icon ${CMAKE_SOURCE_DIR}/data/gdesk.svg
    COMMENT "Génération des icônes PNG")
add_custom_target(gdesk-icons ALL DEPENDS ${CMAKE_BINARY_DIR}/icons/512x512/apps/gdesk.png)

include(GNUInstallDirs)
install(TARGETS gdesk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/gdesk.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/gdesk.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
foreach(size ${GDESK_ICON_SIZES})
    install(FILES ${CMAKE_BINARY_DIR}/icons/${size}x${size}/apps/gdesk.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps)
endforeach()
install(FILES data/io.github.krakenagite.gdesk.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/gdesk.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

option(GDESK_BUILD_TESTS "Compiler les tests" OFF)
if(GDESK_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(gdesk-tests tests/tests.cpp
        src/mime.cpp src/googleauth.cpp src/gmailapi.cpp src/messageview.cpp
        src/settingsdialog.cpp src/theme.cpp src/mainwindow.cpp src/composer.cpp src/setupdialog.cpp
        data/gdesk.qrc)
    target_include_directories(gdesk-tests PRIVATE src)
    target_compile_definitions(gdesk-tests PRIVATE GDESK_VERSION="${PROJECT_VERSION}")
    target_link_libraries(gdesk-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network Qt6::DBus
                                             Qt6::WebEngineCore Qt6::WebEngineWidgets Qt6Keychain::Qt6Keychain)
endif()
