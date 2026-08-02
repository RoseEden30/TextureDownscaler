# Build configuration for the TextureDownscaler SKSE plugin.
# The project name drives the name of the .dll, the .ini and the .log file.

cmake_minimum_required(VERSION 3.21)

project(TextureDownscaler VERSION 2.0.1 LANGUAGES CXX)

# Name of the folder created under the mods folder. Separate from the project
# name so it can carry a space without renaming the files.
set(MOD_FOLDER_NAME "${PROJECT_NAME}" CACHE STRING "Folder name under SKYRIM_MODS_FOLDER")

# Output location, picked up from the environment so this file stays portable.
# Set SKYRIM_FOLDER to a Skyrim Special Edition install, or SKYRIM_MODS_FOLDER
# to the "mods" folder of Mod Organizer 2 or Vortex.
if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()

if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${MOD_FOLDER_NAME}")
endif()

find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME} AUTHOR RoseEden SOURCES plugin.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)

# Symbols in every configuration, so a crash log names a function instead of an
# offset. /Z7 keeps the debug info in the object files; /DEBUG writes the .pdb
# and switches /OPT off, so REF goes back in. ICF stays off, since folded
# functions resolve to the wrong name.
target_compile_options(${PROJECT_NAME} PRIVATE /Z7)
target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:NOICF)

# Deploy the plugin and its settings file after every build.
if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")

    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.ini" "${DLL_FOLDER}/${PROJECT_NAME}.ini"
        VERBATIM
    )

    # Crash Logger SSE reads a .pdb sitting next to the plugin.
    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )
endif()
