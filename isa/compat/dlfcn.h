// ISA's kodi/tools/DllHelper.h includes <dlfcn.h>; newlib has none. Use the Kodi port's declarations,
// which kodi.bin implements in xbmc/platform/switch2/DlStubs.cpp.
#pragma once
#include "/k/xbmc/xbmc/platform/switch2/dlfcn.h"
