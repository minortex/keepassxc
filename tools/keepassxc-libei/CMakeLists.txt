cmake_minimum_required(VERSION 3.16)
project(keepassxc_libei LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core DBus)
pkg_check_modules(LIBEI REQUIRED IMPORTED_TARGET libei-1.0>=1.6)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

add_library(keepassxc-libei SHARED keepassxc-libei.cpp)
target_compile_features(keepassxc-libei PRIVATE cxx_std_17)
target_link_libraries(keepassxc-libei PRIVATE Qt6::Core Qt6::DBus PkgConfig::LIBEI PkgConfig::XKBCOMMON dl)
set_target_properties(keepassxc-libei PROPERTIES OUTPUT_NAME keepassxc-libei)
