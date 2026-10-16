cmake_minimum_required(VERSION 3.16)
project(keepassxc_libei LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core DBus)
set(CMAKE_AUTOMOC ON)
pkg_check_modules(LIBEI REQUIRED IMPORTED_TARGET libei-1.0>=1.6)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XTST REQUIRED IMPORTED_TARGET xtst)

add_library(keepassxc-libei SHARED keepassxc-libei.cpp)
target_compile_features(keepassxc-libei PRIVATE cxx_std_17)
target_link_libraries(keepassxc-libei PRIVATE Qt6::Core Qt6::DBus PkgConfig::LIBEI PkgConfig::XKBCOMMON PkgConfig::X11 PkgConfig::XTST dl)
set_target_properties(keepassxc-libei PROPERTIES OUTPUT_NAME keepassxc-libei)

add_executable(keepassxc-libei-test-autotype test-autotype.cpp)
target_compile_features(keepassxc-libei-test-autotype PRIVATE cxx_std_17)
target_link_libraries(keepassxc-libei-test-autotype PRIVATE Qt6::Core Qt6::DBus PkgConfig::XKBCOMMON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_executable(keepassxc-libei-x11-receiver x11-receiver.cpp)
target_compile_features(keepassxc-libei-x11-receiver PRIVATE cxx_std_17)
target_link_libraries(keepassxc-libei-x11-receiver PRIVATE Qt6::Widgets)
