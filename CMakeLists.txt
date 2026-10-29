cmake_minimum_required(VERSION 3.20)
project(VillageQoL CXX)

set(CMAKE_CXX_STANDARD 17)

# Link the C++ runtime into the DLL, so players need no redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# VillageQoL is a Windows DLL the game loads, under Proton or Wine on Linux as
# well, and village.exe is x64 only.
if(NOT WIN32)
    message(FATAL_ERROR "VillageQoL is a Windows DLL; build it on Windows.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Village in the Shade mods target the x64 village.exe only.")
endif()

# The release version. The DLL's version resource carries it, where Canopy
# Studio reads it, and the mod's manifest reports it to the loader. The release
# workflow sets it from the tag.
set(VILLAGEQOL_RELEASE "1.0.0" CACHE STRING "VillageQoL release version")
# The numeric version: the release's three numbers, or zeros for a build that
# is not a release, such as dev-1a2b3c4.
if(VILLAGEQOL_RELEASE MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(VILLAGEQOL_MAJOR ${CMAKE_MATCH_1})
    set(VILLAGEQOL_MINOR ${CMAKE_MATCH_2})
    set(VILLAGEQOL_PATCH ${CMAKE_MATCH_3})
else()
    set(VILLAGEQOL_MAJOR 0)
    set(VILLAGEQOL_MINOR 0)
    set(VILLAGEQOL_PATCH 0)
endif()

# Canopy: the SDK header, and src/common, which holds the game addresses and
# the feature lists VillageQoL checks before it hooks anything. It comes from
# the git submodule in external/Canopy; name another checkout with
# -DCANOPY_DIR=<folder> to build against local Canopy changes.
set(CANOPY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/Canopy" CACHE PATH "Canopy's source folder")
if(NOT EXISTS "${CANOPY_DIR}/src/common/game_retail.h")
    message(FATAL_ERROR "Canopy's sources are not in ${CANOPY_DIR}. "
                        "Run git submodule update --init, or pass -DCANOPY_DIR=<folder>.")
endif()

# Only what VillageQoL links: no loader, so no vcpkg, and no tools. Canopy's
# core tests stay on, because their address-list check covers this
# repository's sources: each game address a feature uses must be in a list it
# checks.
option(VILLAGEQOL_BUILD_TESTS "Build Canopy's core tests over VillageQoL's sources" ON)
set(CANOPY_BUILD_LOADER OFF)
set(CANOPY_BUILD_TOOLS OFF)
set(CANOPY_BUILD_EXAMPLES OFF)
set(CANOPY_BUILD_TESTS ${VILLAGEQOL_BUILD_TESTS})
set(CANOPY_MOD_SOURCE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE STRING "" FORCE)
if(VILLAGEQOL_BUILD_TESTS)
    enable_testing()
endif()
add_subdirectory("${CANOPY_DIR}" canopy)

add_subdirectory(src)

# One package, installed with cmake --install build --component villageqol:
# mods\VillageQoL.dll in the game folder layout, with the license. Canopy
# Studio installs VillageQoL from this layout.
install(FILES LICENSE DESTINATION . COMPONENT villageqol)
