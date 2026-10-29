# VillageQoL is a mod like any other: it reaches the game's menus, input and
# hooks only through the Canopy API, and installs in mods\.
add_library(VillageQoL SHARED
        plugin.cpp
        host.cpp
        host.h
        features.h
        bgm_info.cpp
        ost_tracklist.h
        quick_save.cpp
        settings_hotkey.cpp
        walk_speed.cpp
        event_fast_forward.cpp
        wheel_zoom.cpp
        key_binding_backup.cpp
        villageqol_version.rc
)

# The release version (see the top CMakeLists.txt), for villageqol_version.rc
# and the manifest.
target_compile_definitions(VillageQoL PRIVATE
        VILLAGEQOL_RELEASE="${VILLAGEQOL_RELEASE}"
        VILLAGEQOL_MAJOR=${VILLAGEQOL_MAJOR}
        VILLAGEQOL_MINOR=${VILLAGEQOL_MINOR}
        VILLAGEQOL_PATCH=${VILLAGEQOL_PATCH})

# Translated UI strings are written as UTF-8 literals.
if(MSVC)
    target_compile_options(VillageQoL PRIVATE /utf-8)
endif()

target_link_libraries(VillageQoL PRIVATE
        VillageCommon
        CanopyApi
)

install(TARGETS VillageQoL RUNTIME DESTINATION mods COMPONENT villageqol)
