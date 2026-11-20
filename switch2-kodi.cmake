# Toolchain wrapper: libounce's Switch 2 toolchain plus our dependency prefix.
include(/opt/devkitpro/libounce/cmake/Switch2.cmake)
list(PREPEND CMAKE_FIND_ROOT_PATH /k/prefix)
set(CMAKE_PREFIX_PATH /k/prefix ${CMAKE_PREFIX_PATH})
# Our pkg-config wrapper searches /k/prefix before the devkit portlibs.
set(PKG_CONFIG_EXECUTABLE ${CMAKE_CURRENT_LIST_DIR}/pkg-config-switch2 CACHE FILEPATH "" FORCE)
set(CMAKE_USER_MAKE_RULES_OVERRIDE ${CMAKE_CURRENT_LIST_DIR}/switch2-flags-override.cmake)
