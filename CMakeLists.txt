cmake_minimum_required(VERSION 3.16)

# Project-wide compiler flags: treat warnings seriously, as a production firmware should.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rw1990_programmer)
