# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(chd2iso C)
# libchdr is fetched next to this file by the caller (git clone https://github.com/rtissera/libchdr)
set(LIBCHDR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libchdr" CACHE PATH "libchdr checkout")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${LIBCHDR_DIR} libchdr EXCLUDE_FROM_ALL)
add_executable(chd2iso chd2iso.c)
target_link_libraries(chd2iso PRIVATE chdr-static)
