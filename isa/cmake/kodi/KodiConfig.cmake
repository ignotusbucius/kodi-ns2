# Stand-in for the kodi-config.cmake a Kodi install provides (xbmc/cmake/KodiConfig.cmake.in),
# filled with the Switch 2 port's values. It lets inputstream.adaptive's own CMakeLists build
# unchanged, but with BUILD_SHARED_LIBS OFF, so build_addon() makes a static archive.
set(APP_NAME kodi)
set(APP_NAME_LC kodi)
set(APP_NAME_UC KODI)
set(APP_VERSION_MAJOR 21)
set(APP_VERSION_MINOR 3)
set(KODI_SOURCE_TREE /k/xbmc)
set(KODI_INCLUDE_DIR ${KODI_SOURCE_TREE}/xbmc/addons/kodi-dev-kit/include/kodi)
set(KODI_LIB_DIR ${KODI_SOURCE_TREE}/cmake/scripts/common)
set(KODI_DATA_DIR ${KODI_SOURCE_TREE})
set(APP_RENDER_SYSTEM gles)
list(APPEND CMAKE_MODULE_PATH ${KODI_SOURCE_TREE}/cmake/scripts/common)

# ARCH_DEFINES from xbmc/cmake/scripts/switch2/ArchSetup.cmake, plus the add-on marker.
add_definitions(-DTARGET_POSIX -DTARGET_SWITCH2 -D__SWITCH__ -D__SWITCH2__ -DBUILD_KODI_ADDON)

set(CORE_SYSTEM_NAME switch2)
set(PLATFORM switch2)
set(PLATFORM_TAG switch2)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(AddonHelpers)

# Kodi's depends prefix normally sits on the system include path; ours is /k/isa/prefix.
include_directories(SYSTEM /k/isa/prefix/include)
# Switch 2 static build: compile out every decrypter (patches/0001-isa-no-drm.patch).
if(ISA_NO_DRM)
  add_definitions(-DISA_NO_DRM)
endif()
# The same newlib gap-fillers Kodi's own targets get (xbmc/cmake/scripts/switch2/ArchSetup.cmake):
# timegm() and friends are implemented in kodi.bin (xbmc/platform/switch2/PosixCompat.cpp).
# <dlfcn.h> forwards to the port's stub declarations (xbmc/platform/switch2/dlfcn.h).
include_directories(SYSTEM /k/isa/compat /k/xbmc/xbmc/platform/switch2/compat)
add_compile_options(-include /k/xbmc/xbmc/platform/switch2/compat/switch2_posix.h)
