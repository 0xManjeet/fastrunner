cmake_minimum_required(VERSION 3.16)
project(fastapprunner VERSION 1.2)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)

# THE FIX: Tell KDE's ECM to strictly use Qt6, preventing the Qt5 fallback crash.
set(QT_MAJOR_VERSION 6 CACHE STRING "Force Qt 6" FORCE)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)

find_package(Qt6 COMPONENTS Core Gui DBus Network REQUIRED)
find_package(KF6 COMPONENTS Runner Service KIO I18n CoreAddons REQUIRED)

add_library(fastapprunner MODULE fastapprunner.cpp)

target_link_libraries(fastapprunner
    Qt6::Core
    Qt6::Gui
    Qt6::DBus
    Qt6::Network
    KF6::Runner
    KF6::Service
    KF6::KIOGui
    KF6::I18n
    KF6::CoreAddons
)

install(TARGETS fastapprunner DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf6/krunner)