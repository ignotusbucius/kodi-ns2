<p align="center">
  <img src="docs/img/kodi-logo.png" width="150" alt="Kodi logo">
</p>

<h1 align="center">Kodi for Nintendo Switch 2</h1>

<p align="center">
  Kodi 21.3 "Omega" as a native Switch 2 homebrew app, launched from hbmenu under kaiHEN.<br>
  Hardware video decoding, 4K HDR10 and AV1, USB drives with hot-plug, Python add-ons and skins, docked and handheld.
</p>

<p align="center">
  <img alt="status" src="https://img.shields.io/badge/status-beta-yellow">
  <img alt="release" src="https://img.shields.io/badge/release-rev%2039e-informational">
  <img alt="kodi" src="https://img.shields.io/badge/Kodi-21.3%20Omega-17b2e7">
  <img alt="platform" src="https://img.shields.io/badge/platform-Switch%202%20(kaiHEN)-e60012">
  <img alt="hwdec" src="https://img.shields.io/badge/HW%20decode-H.264%20%7C%20VP9%20%7C%20AV1%20%7C%20VP8-success">
  <img alt="video" src="https://img.shields.io/badge/video-2160p%20HDR10%20%7C%20AV1-success">
  <img alt="usb" src="https://img.shields.io/badge/USB-hot--plug-087f68">
  <img alt="python" src="https://img.shields.io/badge/Python-3.11%20add--ons-3776ab">
  <img alt="license" src="https://img.shields.io/badge/license-GPL--2.0--or--later-blue">
</p>

<p align="center">
  <img src="docs/img/home-screen.png" width="49%" alt="Kodi home screen on Switch 2">
  <img src="docs/img/python-addons.png" width="49%" alt="Bundled Python video add-ons">
</p>

## What works

Tested on a retail Switch 2. Handheld works as well as docked.

| Area | State |
|---|---|
| Hardware decoding | The console's own video decoder plays **8-bit H.264 and VP9 up to 1080p, and AV1 and VP8 up to 4K at 30 fps**. Quality switches in adaptive streams stay on the hardware decoder. Anything else falls back to the CPU on its own. |
| Video | H.264, HEVC, **AV1** (dav1d), VP9, VP8 and MPEG-2. 8- and 10-bit, **up to 2160p in real time**, including **HDR10** and 4K AV1 10-bit. 1080p60 plays smoothly. |
| HDR | HDR10 plays at full speed and is tone-mapped to SDR. |
| USB drives | Plug a USB stick or drive in **while Kodi runs** and it shows up under *Files*. Pull it out at any time, even during playback: Kodi stops the video and stays responsive. Plug it back in and play again. Drives are read-only. |
| Streaming | Streaming add-ons play over the internet. Third-party add-ons work too: **POV with a Premiumize account** was used for real films and shows. inputstream.adaptive (HLS/DASH, no DRM) is built in. HTTP sources on your network play, including a 31 Mb/s Blu-ray remux with fast seeking. |
| Python add-ons | CPython 3.11 and **Pillow** are built in. **41 add-ons from the official Kodi repository come pre-installed**, among them YouTube, Internet Archive, TED Talks, Vimeo, Twitch, Pluto TV, Red Bull TV, NASA, Arte, Dailymotion, iPlayer and TMDb Helper. Heavy add-ons that start many threads no longer run the console out of them. |
| Skins | Estuary is built in. **Arctic Horizon 2** installs from its repository and works. |
| Audio | PCM, AC3, E-AC3, DTS, DTS-HD MA, AAC (up to 7.1), FLAC, Opus and MP3, mixed down to stereo. |
| Subtitles | Embedded SRT and ASS (with styles), external `.srt` files, and switching audio and subtitle tracks during playback. |
| Remote control | Kodi's web server and JSON-RPC: play, pause, stop and open files from a phone or PC. Off by default; turn it on under *Settings → Services → Control*. |
| Music and pictures | MP3, FLAC and AAC playback with playlists, and picture slideshows. |
| Input | Controller with key repeat: A select, B back, X context menu, Y info, +/− menu and OSD, L/R page, ZL/ZR rewind and fast-forward. The on-screen keyboard works for text entry. |
| Display | Docked and handheld. The GUI follows the console's output size. The screen doesn't dim or sleep while video plays. |
| Quitting | Quit is quick even when add-on services are still busy. If a script refuses to stop, Kodi closes cleanly to HOME instead of freezing. |

<p align="center">
  <img src="docs/img/playback-2160p-hdr10.png" width="49%" alt="2160p HEVC 10-bit HDR10 test clip playing">
  <img src="docs/img/files-view.png" width="49%" alt="Kodi file browser">
</p>

## Limitations

- **HEVC is decoded on the CPU.** It still plays 4K HDR10 test files in real time, but very high-bitrate 4K remuxes
  haven't been tried. 10-bit H.264 and VP9 above 1080p also use the CPU.
- **Stereo by default**: surround tracks are decoded and mixed down, and there's no passthrough. An experimental
  5.1 PCM output (*Settings → System → Audio*, off by default) exists but hasn't been checked on a receiver.
  There's no HDR output either; HDR10 is tone-mapped.
- **USB**: read-only, FAT32 and exFAT. A video doesn't resume by itself after the drive comes back; start it again
  from *Files*. Favourites, library sources and resume points on a USB drive don't carry over once it's unplugged.
- **Binary add-ons**: the platform has no `dlopen`, so only add-ons linked into the app work (inputstream.adaptive).
  No DRM or Widevine, and no PVR clients yet.
- **Not tested yet**:
  - NFS against a real server;
  - PGS subtitles, VC-1 and TrueHD/Atmos;
  - touch, sleep/resume, and the 4K and 720p output modes;
  - skins other than Estuary and Arctic Horizon 2.
- **Not available**: SMB and UPnP aren't built, and AirPlay is off.

## Install

You need a Switch 2 running kaiHEN with hbmenu.

1. Download `kodi-ns2-rev39e.zip` from [Releases](https://github.com/ignotusbucius/kodi-ns2/releases), or build it
   yourself (below).
2. Copy the `switch` folder from the zip to the root of the SD card, merging it with the one already there:
   ```
   sdmc:/switch/kodi.nro
   sdmc:/switch/kodi/        addons/, media/, system/, userdata/
   ```
3. Start *Kodi* from hbmenu. The first start takes longer while Kodi creates its databases and scans add-ons.
4. Add media under *Videos → Files → Add videos…*. SD paths start with `/`, e.g. `/switch/kodi-media/`. A plugged-in
   USB drive appears in the same list. You can also use `http://` or `nfs://` sources on your network.

**Upgrading:** unpack the new zip over the old one. Your settings, sources, add-ons and libraries live in
`sdmc:/switch/kodi/.kodi/`, which the zip never contains, so they're kept. The log is at
`sdmc:/switch/kodi/.kodi/temp/kodi.log`.

Kodi only writes under `sdmc:/switch/kodi/`. Every build is checked to make sure it links no NAND, save-data or
system-settings calls, and that the USB code linked in is exactly the reviewed read-only library.

## Build

You need a Linux host with Podman or Docker. The build uses the
[ounce-devkit](https://github.com/ouncebrew/ounce-devkit) image (devkitPro, libounce and OunceVK).

```bash
# devkit images, once
git clone https://github.com/ouncebrew/ounce-devkit && podman build -t ounce-devkit ounce-devkit
podman build -t kodi-switch2-devkit -f Containerfile .

# Kodi 21.3 plus this port
./bootstrap.sh              # clones xbmc/xbmc at 21.3-Omega into ./xbmc and applies the patch

# inputstream.adaptive (static) and the pre-installed add-ons
./isa/build.sh all          # -> isa/out/
./addons-bundle/fetch.sh    # 41 add-on zips from the official Kodi mirror, sha256-checked, plus the YouTube patch

# Kodi itself
./build.sh all              # hosttools, deps, configure, build, nro  -> out/switch/
./release.sh mybuild        # optional: one-zip SD install -> dist/kodi-ns2-mybuild.zip
```

Stages can be run one at a time with `./build.sh hosttools|deps|usbfs2|configure|build|nro`. A diagnostics build goes
in a separate tree: `BUILD_DIR=build-diag OUT_DIR=out-diag SWITCH2_DIAG=ON ./build.sh configure build nro`.

The libraries the devkit lacks are built statically into `prefix/`:

- libiconv 1.17 and OpenSSL 3.0.22;
- SQLite 3.49.2, TinyXML 2.6.2 and libcdio 2.1.0;
- dav1d 1.4.1 and FFmpeg 6.0.1;
- CPython 3.11.7 with Pillow 8.4.0, libnfs 5.0.2 and libmicrohttpd 0.9.77;
- fstrcmp and a small libuuid;
- `usbfs2/`, the read-only USB library (below). Its build is reproducible, and `build.sh` refuses any archive that
  doesn't match the pinned SHA-256.

Each has its patches under `deps/patches/`. After `./bootstrap.sh`, `hosttools/check-net-select.sh`
runs the host tests for the socket and thread code; `hosttools/tests/` also has the decoder-handover test.

## How the port works

The port adds a `switch2` platform to Kodi's build:

- CMake platform files and `xbmc/platform/switch2`;
- an EGL/GLES window system on the default NWindow;
- an `audout` audio sink;
- a video codec for the console's hardware decoder, with a software fallback;
- read-only USB storage: `usbfs2` talks to the console's `usb:hs` service and reads FAT32 and exFAT through FatFs;
- a statically linked CPython;
- inputstream.adaptive, linked into the app.

The rest is small shims for places where the console's newlib/libounce runtime behaves differently from Linux.
Each of these turned up on hardware:

| On the Switch 2 | Effect on Kodi | Fix |
|---|---|---|
| `read()` past end-of-file returns EIO instead of 0 | SQLite saw every new database as corrupt | Read wrapper clamps to the file size |
| `stat()` on a file open for writing returns EIO | SQLite `IOERR_FSTAT` | `stat`/`lstat` fall back to the entry type and our own fd |
| A second `open()` of a file open for writing fails | Kodi's second database connection failed | SQLite shared cache; file cache kept in RAM |
| No POSIX advisory locks | SQLite lock errors | `unix-dotfile` VFS |
| `pthread_detach()` always fails | `std::thread::detach` threw and Kodi aborted | Detached threads are handed to a reaper that joins them as soon as they end |
| The app can only have a few dozen threads | Add-ons like TMDb Helper and POV ran Kodi out of threads | Ended threads are freed at once, and a failed thread start no longer takes Kodi down |
| `select()` refuses descriptors above 63 | HTTPS streams failed once enough sockets were open | `select()` rebuilt on `poll()`; curl waits with `curl_multi_wait` |
| Kodi's exit doesn't end the loader's process | Leftover threads crashed the next launch | Kodi waits for its threads, or ends the process cleanly back to HOME |
| Default thread stack is 128 KiB | Risk of overflow in deep call chains | 1 MiB default stack |
| `CLOCK_MONOTONIC_RAW` isn't supported | Garbage media clock; a 30 s clip finished in 3 s | Use `CLOCK_MONOTONIC` and map the other clock IDs |
| `ioctl(FIOCLEX)` works only on sockets | Python couldn't open any add-on script | `os.set_inheritable` is a no-op (CPython patch) |
| `fcntl` supports only `F_GETFL`/`F_SETFL` | Python and Kodi file locking failed | `fcntl` wrapper for dup, `FD_CLOEXEC` and locks |
| `socketpair()` isn't implemented | The web server couldn't start | Emulated over loopback TCP/UDP |
| The first OpenSSL method built on a non-main thread kills the process | Python add-ons died a few seconds after start | Every OpenSSL algorithm is fetched once on the main thread at startup (~260 ms) |
| The hardware decoder takes one stream at a time | A quality switch fell back to the CPU | The new stream takes over the decoder as the old one closes |
| No USB mass-storage support in the SDK | No USB drives | `usbfs2`, a small read-only USB storage library |
| The console dims and sleeps during long playback | The screen went dark mid-film | Media-playback state is set while video plays |
| No `dlopen`, `/proc`, `sysinfo`, `uname` or `mmap` | Build and link errors | Stubs, compatibility headers and statically linked add-ons |

CPU exceptions don't reach homebrew on this firmware, so problems are tracked down with diagnostics builds, which
write breadcrumbs to the SD card.

## Credits and license

- [Kodi](https://kodi.tv) is © Team Kodi and licensed GPL-2.0-or-later. Kodi® and the Kodi logo are trademarks of
  the XBMC Foundation. This is an unofficial port, not endorsed by Team Kodi or the XBMC Foundation.
- Switch 2 port by [@ignotusbucius](https://github.com/ignotusbucius), under the same license as Kodi.
- kaiHEN, libounce, OunceVK and ounce-devkit by [ouncebrew](https://github.com/ouncebrew); devkitPro and libnx.
- The pre-installed add-ons come from the official Kodi repository, each under its own license (see their
  `addon.xml`). YouTube carries one small patch, in `addons-bundle/patches/`.
- FFmpeg, dav1d, OpenSSL, SQLite, CPython, Pillow, libnfs, libmicrohttpd, inputstream.adaptive, Bento4, libiconv,
  libcdio, TinyXML, fstrcmp, Mesa and FatFs (in `usbfs2/third_party/fatfs`) are under their own licenses.
- Not affiliated with Nintendo. Use it on hardware you own.
