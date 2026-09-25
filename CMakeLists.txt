cmake_minimum_required(VERSION 3.16)
project(gdesk VERSION 2.0.0 LANGUAGES CXX)

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
    data/gdesk.qrc)
target_compile_definitions(gdesk PRIVATE GDESK_VERSION="${PROJECT_VERSION}")
target_link_libraries(gdesk PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus
                                    Qt6::WebEngineCore Qt6::WebEngineWidgets Qt6Keychain::Qt6Keychain)

include(GNUInstallDirs)
install(TARGETS gdesk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/gdesk.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/gdesk.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

option(GDESK_BUILD_TESTS "Compiler les tests" OFF)
if(GDESK_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(gdesk-tests tests/tests.cpp
        src/mime.cpp src/googleauth.cpp src/gmailapi.cpp src/messageview.cpp)
    target_include_directories(gdesk-tests PRIVATE src)
    target_compile_definitions(gdesk-tests PRIVATE GDESK_VERSION="${PROJECT_VERSION}")
    target_link_libraries(gdesk-tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network
                                             Qt6::WebEngineCore Qt6::WebEngineWidgets Qt6Keychain::Qt6Keychain)
endif()
