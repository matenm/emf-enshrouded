# An EMF mod. Build it with Visual Studio 2022 ("Open a local folder" on this folder) or on the command line:
#   cmake --preset release && cmake --build --preset release
# The mod lands in build/release/<name>/ (the DLL and its mod.json): copy that folder into <game>/emf/mods/. Set
# EMF_GAME_DIR (your Enshrouded folder) to copy it there after every build:
#   cmake --preset release -DEMF_GAME_DIR="C:/SteamLibrary/steamapps/common/Enshrouded"
cmake_minimum_required(VERSION 3.21)
project(mymod LANGUAGES CXX)  # your mod's id: change it here only (1-48 of a-z 0-9 _ . -)
string(LENGTH "${PROJECT_NAME}" MOD_ID_LENGTH)
if(NOT PROJECT_NAME MATCHES "^[a-z0-9_.-]+$" OR MOD_ID_LENGTH GREATER 48)
  message(FATAL_ERROR "project(${PROJECT_NAME}): the project name is your mod's id, and EMF only loads ids of 1-48 "
    "characters from a-z 0-9 _ . - (lower case, no spaces), e.g. project(my_mod LANGUAGES CXX).")
endif()

set(EMF_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include" CACHE PATH "EMF SDK include folder (holds emf/emf.hpp)")
set(EMF_GAME_DIR "" CACHE PATH "Enshrouded folder: the mod is copied into <game>/emf/mods/<name> after each build (optional)")
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

set(MOD_DIR "${CMAKE_BINARY_DIR}/${PROJECT_NAME}")  # the mod's folder, as it goes into <game>/emf/mods/
add_library(${PROJECT_NAME} SHARED src/mymod.cpp)  # add more .cpp files here
target_include_directories(${PROJECT_NAME} PRIVATE "${EMF_SDK_DIR}")
target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_compile_definitions(${PROJECT_NAME} PRIVATE MOD_ID="${PROJECT_NAME}")  # the code's mod id (log, settings)
set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${MOD_DIR}>")

# mod.json next to the DLL, with "id" set from project() above
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/mod.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mod.json" MOD_JSON)
string(JSON MOD_JSON SET "${MOD_JSON}" id "\"${PROJECT_NAME}\"")
file(WRITE "${MOD_DIR}/mod.json" "${MOD_JSON}\n")

if(EMF_GAME_DIR)
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EMF_GAME_DIR}/emf/mods/${PROJECT_NAME}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${MOD_DIR}/mod.json"
            "${EMF_GAME_DIR}/emf/mods/${PROJECT_NAME}/"
    COMMENT "copying ${PROJECT_NAME} into ${EMF_GAME_DIR}/emf/mods (close the game first)")
endif()
