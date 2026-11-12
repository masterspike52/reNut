# A native RexFabric mod. It needs nothing from the game or the SDK, only RexFabric's include folder.
#
#   cmake -B build -DREXFABRIC_INCLUDE=<RexFabric>/include
#   cmake --build build
#
# Copy the resulting my_mod.asi into mods/<YourMod>/ beside the game.

cmake_minimum_required(VERSION 3.20)
project(my_mod LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(REXFABRIC_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../include" CACHE PATH "RexFabric's include folder")

add_library(my_mod MODULE my_mod.cpp)
target_include_directories(my_mod PRIVATE ${REXFABRIC_INCLUDE})
set_target_properties(my_mod PROPERTIES PREFIX "" SUFFIX ".asi")
