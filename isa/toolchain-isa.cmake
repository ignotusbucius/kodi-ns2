# Toolchain for the static inputstream.adaptive build: libounce's Switch 2 toolchain (the one
# ../switch2-kodi.cmake uses for Kodi) plus our own dependency prefix /k/isa/prefix.
# Kodi's /k/prefix is deliberately NOT searched: ISA must not pick up Kodi's copies of shared deps.
include(/opt/devkitpro/libounce/cmake/Switch2.cmake)
list(PREPEND CMAKE_FIND_ROOT_PATH /k/isa/prefix)
set(CMAKE_PREFIX_PATH /k/isa/prefix ${CMAKE_PREFIX_PATH})
set(CMAKE_USER_MAKE_RULES_OVERRIDE ${CMAKE_CURRENT_LIST_DIR}/flags-override.cmake)
