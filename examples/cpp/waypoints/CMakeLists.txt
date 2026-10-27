# waypoints: an EMF example mod in C++. Build it with Visual Studio 2022 ("Open a local folder" on this folder) or on
# the command line:
#   cmake --preset release && cmake --build --preset release
# The mod lands in build/release/waypoints/ (the DLL and its mod.json): copy that folder into <game>/emf/mods/. Set
# EMF_GAME_DIR (your Enshrouded folder) to copy it there after every build:
#   cmake --preset release -DEMF_GAME_DIR="C:/SteamLibrary/steamapps/common/Enshrouded"
cmake_minimum_required(VERSION 3.21)
project(waypoints LANGUAGES CXX)

# The SDK sits three folders up in the EMF package (examples/cpp/waypoints -> sdk/include), and in the repository too.
set(EMF_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/include" CACHE PATH "EMF SDK include folder (holds emf/emf.hpp)")
set(EMF_GAME_DIR "" CACHE PATH "Enshrouded folder: the mod is copied into <game>/emf/mods/waypoints after each build (optional)")
if(NOT EXISTS "${EMF_SDK_DIR}/emf/emf.hpp")
  message(FATAL_ERROR "EMF SDK not found: ${EMF_SDK_DIR}/emf/emf.hpp is missing. Set EMF_SDK_DIR to the SDK's include folder.")
endif()
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "EMF mods are 64-bit Windows DLLs: build with MSVC x64 (Visual Studio 2022, or the "
    "\"x64 Native Tools Command Prompt for VS 2022\"). Delete the build folder before trying again: it remembers "
    "the compiler it found first.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: nothing else to install

set(MOD_DIR "${CMAKE_BINARY_DIR}/waypoints")  # the mod's folder, as it goes into <game>/emf/mods/
add_library(waypoints SHARED src/waypoints.cpp)
target_include_directories(waypoints PRIVATE "${EMF_SDK_DIR}")
target_compile_options(waypoints PRIVATE /W4 /permissive- /utf-8 /EHsc)
set_target_properties(waypoints PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${MOD_DIR}>")
configure_file(mod.json "${MOD_DIR}/mod.json" COPYONLY)  # next to the DLL (copied again when it changes)

if(EMF_GAME_DIR)
  add_custom_command(TARGET waypoints POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EMF_GAME_DIR}/emf/mods/waypoints"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:waypoints>" "${MOD_DIR}/mod.json"
            "${EMF_GAME_DIR}/emf/mods/waypoints/"
    COMMENT "copying waypoints into ${EMF_GAME_DIR}/emf/mods (close the game first)")
endif()
