# inputstream.adaptive in the Switch 2 Kodi build — integration steps

Status: ISA builds statically and links into the current Kodi objects with no undefined or duplicate
symbols (`linktest/relink-kodi.sh`). Nothing below has been applied to `xbmc/` or `build.sh` yet,
and nothing has run on a console.

## What exists

| Path | What |
|---|---|
| `isa/build.sh` | `fetch` → `deps` → `isa` → `pack` → `data` → `linktest` (`all` runs them in that order) |
| `isa/out/lib/libkodiaddon_inputstream.adaptive.a` | one prelinked object: ISA 21.5.26-Omega + mpegts + webm_parser + bento4 + pugixml. Its only global definitions are `isa_ADDON_Create`, `isa_ADDON_GetTypeVersion` and `isa_ADDON_GetTypeMinVersion` |
| `isa/out/inputstream.adaptive/` | add-on data folder (addon.xml, resources/, icon, fanart, placeholder `inputstream.adaptive.so`) |
| `xbmc/xbmc/platform/switch2/StaticAddons.{h,cpp}` | table-driven registry: library file name → entry-point table |
| `xbmc/xbmc/platform/switch2/DlStubs.cpp` | `dlopen` asks the registry, `dlsym` on that handle returns the renamed entry points. The registry is referenced **weakly**, so this file links and behaves as before until StaticAddons.cpp is added to the build |

## 1. Compile the registry (xbmc/xbmc/platform/switch2/CMakeLists.txt)

```cmake
set(SOURCES PlatformSwitch2.cpp
            CrashHandler.cpp
            DlStubs.cpp
            PosixCompat.cpp
            StartupProbe.cpp
            StaticAddons.cpp
            SystemInfoSwitch2.cpp)
set(HEADERS PlatformSwitch2.h
            PosixCompat.h
            StaticAddons.h
            dlfcn.h)

if(ENABLE_STATIC_ISA)
  set_source_files_properties(StaticAddons.cpp PROPERTIES
                              COMPILE_DEFINITIONS KODI_STATIC_ADDON_INPUTSTREAM_ADAPTIVE)
endif()

core_add_library(platform_switch2)
```

Without `KODI_STATIC_ADDON_INPUTSTREAM_ADAPTIVE` the table is empty and every dlopen still fails.

## 2. Link the add-on (xbmc/cmake/scripts/switch2/ArchSetup.cmake)

Add after the existing `CMAKE_CXX_STANDARD_LIBRARIES` line:

```cmake
# inputstream.adaptive, linked statically (homebrew/kodi/isa/INTEGRATE.md).
option(ENABLE_STATIC_ISA "Link inputstream.adaptive into kodi.bin" ON)
set(STATIC_ISA_LIBRARY /k/isa/out/lib/libkodiaddon_inputstream.adaptive.a CACHE FILEPATH "")
if(ENABLE_STATIC_ISA)
  if(NOT EXISTS ${STATIC_ISA_LIBRARY})
    message(FATAL_ERROR "${STATIC_ISA_LIBRARY} missing: run homebrew/kodi/isa/build.sh all")
  endif()
  set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} ${STATIC_ISA_LIBRARY}")
endif()
```

Link order: the archive must come **after** `platform_switch2.a` (which references `isa_ADDON_*` from
StaticAddons.o). `CMAKE_CXX_STANDARD_LIBRARIES` is appended after `LINK_LIBRARIES`, so it does; this is
the order `linktest/relink-kodi.sh` verified. It must not be inside `--whole-archive` (not needed: the
registry references it). No new defines are needed for Kodi's other sources, and no `-l` flags: the
add-on's undefined symbols are libc/libm/libstdc++/pthread (already on Kodi's line) plus `timegm` from
`PosixCompat.cpp`. Kodi's `-Wl,--wrap=pthread_create` etc. apply to ISA too (its threads get the port's
default 1 MiB stacks).

Rebuild order: `isa/build.sh all` (static libs), then `./build.sh configure && ./build.sh build`.
`isa/build.sh` never touches `build/`; rerun `pack` after any ISA source change.

## 3. Package the data folder (package.sh)

Add after the data-tree copy loop (`for d in addons media system userdata`):

```bash
# inputstream.adaptive: code linked into kodi.bin, data folder from isa/build.sh data.
ISA=isa/out/inputstream.adaptive
[ -f "$ISA/addon.xml" ] && cp -r "$ISA" "$OUT/kodi/addons/inputstream.adaptive"
```

It goes to `special://xbmc/addons` (= `sdmc:/switch/kodi/addons`). `system/addon-manifest.xml` already
lists `<addon optional="true">inputstream.adaptive</addon>`, so it is enabled on first start
(CAddonDatabase::SyncInstalled) and the user can disable it.

## Why the addon.xml looks like this

- `library="inputstream.adaptive.so"` (plain attribute): `CAddonInfoBuilder::ParseXMLTypes` reads
  `library` before the per-OS attribute. On TARGET_SWITCH2, `GetPlatformLibraryName()` matches no
  `#if` branch; the shipped object compiles it to `return nullptr` (checked by disassembly), so
  `library_linux`/`library_switch2` would be ignored. The `.so` suffix matches
  `GetSharedLibrarySuffix()` (".so" in this build), so the add-on is marked binary.
- `<platform>all</platform>`: `PlatformSupportsAddon()` only accepts "all" on TARGET_SWITCH2.
- Placeholder file `inputstream.adaptive.so`: `CAddonDll::GetDllPath()` returns "" unless the library
  file exists. Then `SoLoader::Load()` calls
  `dlopen("sdmc:/switch/kodi/addons/inputstream.adaptive/inputstream.adaptive.so")` (TranslatePath of
  special://xbmc/addons/...). The registry matches the last path component only, so the same works
  from special://home/addons.
- `<requires>` is the block Kodi's own `build_addon()` generated from versions.h (global.main 2.0.2,
  general 1.0.5, gui 5.15.0, filesystem 1.1.9, tools 1.0.4, instance.inputstream 3.3.0,
  instance.videocodec 2.1.0). These match the kodi.binary.* add-ons in `out/switch/kodi/addons`.

## Optional Kodi source fix (not needed by this add-on)

`xbmc/addons/addoninfo/AddonInfoBuilder.cpp` `GetPlatformLibraryName()` leaves `libraryName`
uninitialised on TARGET_SWITCH2 (undefined behaviour; GCC currently emits `return nullptr`). The minimal fix:

```cpp
#elif defined(TARGET_SWITCH2)
  libraryName = element->Attribute("library_switch2");
#else
  libraryName = nullptr;
#endif
```

and `"switch2"` in `PlatformSupportsAddon()`. With that, `library_switch2=` / `<platform>switch2</platform>`
would also work. The data folder does not depend on it.

## Adding another static add-on

1. Build it as a static archive the way `isa/build.sh isa` does (a KodiConfig.cmake with
   `BUILD_SHARED_LIBS OFF`), then `pack` it with its own prefix (`ld -r --force-group-allocation`,
   `objcopy --redefine-sym ADDON_Create=pfx_ADDON_Create ...`, then `--keep-global-symbol` for the three
   renamed names only).
2. In StaticAddons.cpp: `KODI_STATIC_ADDON_DECLARE(pfx)` plus a row
   `KODI_STATIC_ADDON_ROW("<library>", pfx)` behind its own define.
3. Data folder: `library="<library>"`, `<platform>all</platform>`, and a placeholder file of that name.
Localising everything except the entry points is what lets two add-ons built from the same kodi-dev-kit
headers coexist: each keeps its own `kodi::addon::CPrivateBase::m_interface` and its own copies of the
inline/template code.
