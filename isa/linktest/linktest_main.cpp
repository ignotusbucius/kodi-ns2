// Link test for the static add-on registry: does what Kodi's SoLoader + DllAddon do on Switch 2
// (dlopen of the computed library path, dlsym of the three entry points), with no Kodi code.
#include "dlfcn.h"

#include <cstdio>

int main()
{
  void* h = dlopen("sdmc:/switch/kodi/addons/inputstream.adaptive/inputstream.adaptive.so", RTLD_LAZY);
  if (!h)
  {
    std::printf("dlopen failed: %s\n", dlerror());
    return 1;
  }
  using TypeVersionFn = const char* (*)(int);
  auto ver = reinterpret_cast<TypeVersionFn>(dlsym(h, "ADDON_GetTypeVersion"));
  auto minVer = reinterpret_cast<TypeVersionFn>(dlsym(h, "ADDON_GetTypeMinVersion"));
  void* create = dlsym(h, "ADDON_Create");
  void* missing = dlsym(h, "ADDON_Destroy");
  std::printf("create=%p main=%s min=%s missing=%p(%s)\n", create, ver ? ver(0) : "-",
              minVer ? minVer(0) : "-", missing, dlerror());
  void* other = dlopen("sdmc:/switch/kodi/addons/pvr.iptvsimple/pvr.iptvsimple.so", RTLD_LAZY);
  std::printf("other=%p (%s)\n", other, dlerror());
  dlclose(h);
  return create && ver && minVer && !missing && !other ? 0 : 2;
}
