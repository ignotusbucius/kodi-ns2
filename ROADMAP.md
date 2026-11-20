# Roadmap

State on 2026-11-21: **rev 12** (`out/switch/kodi.nro` built from `patches/kodi-21.3-switch2.patch`) boots to the
Estuary home screen, takes pad input, plays local and HTTP-streamed media in real time up to 2160p, seeks, and
closes cleanly. Evidence and test details: [TESTING.md](TESTING.md).

## Next tasks (ordered)

1. **Replicate the real-media run** (one launch so far): fresh launch, play the 4 HTTP files again,
   time-seek test (Right ×4 / Left ×2) on the TV episode, fetch `kodi.log`.
2. **Audible-audio check** (needs the operator's ears or an audio capture): PCM / AC3 5.1 / DTS 5.1 / DTS-HD MA /
   E-AC3 / FLAC / AAC 7.1 downmix to the speaker; confirm channel mapping of the 5.1→2.0 downmix.
3. **4K HDR10 at full speed**: stop the per-clip `BaseYUV2RGBGLSLShader - unsupported format YV12 10bit` →
   shader rebuild loop (~6 rebuilds per HDR clip; 2160p HEVC10 runs ~10 % slow). Then measure frame drops
   (enable Kodi's player debug overlay / drop counters).
4. **Hardware video decode** (NVDEC on the Switch 2 GPU via nvdrv). Today everything is FFmpeg software decode on
   the CPU; needed for high-bitrate 4K HEVC and battery life.
5. **Docked / TV output**: resolution follows the dock (currently fixed at start, GUI clamped to 1080p); 4K GUI
   output, refresh-rate switching, HDR passthrough if the display path allows it.
6. **Audio passthrough / multichannel**: audout is stereo S16 48 kHz; investigate audren / HDMI multichannel when
   docked.
7. **Network sources**: SMB / NFS (disabled in v1), UPnP, web server / JSON-RPC remote (disabled for the offline
   first runs), add-on repository (disabled).
8. **Python add-ons**: cross-build CPython for libounce, re-enable `ENABLE_PYTHON` (scrapers, YouTube-style
   plugins). Binary add-ons need a static-registration scheme (no `dlopen` on this platform).
9. **Reviewer minor items still open**: crash handler can deadlock if the faulting thread holds libnx's fs
   session-manager mutex; global `stat()` fallback treats every EIO as "exists"; `clock_gettime` EIO fallback to
   `armGetSystemTick()`; `boot.txt` / `sqlite.log` instrumentation can be trimmed; debug `loglevel` off by default.
10. **Long-session stability**: 2-hour playback, sleep/resume (HOME → back), thermal, memory growth (1 MiB default
    thread stacks × threads).
11. **Packaging**: proper NRO icon/NACP (Kodi logo icon done), one-zip install for `sdmc:/switch/`,
    first-run `sources.xml` helper, README for end users.

## Not tested yet

- Touch input on the home screen; Joy-Con detached / Pro Controller; keyboard entry (on-screen keyboard).
- Subtitles (SRT / ASS / PGS), multiple audio tracks switching, chapters UI.
- Music library, pictures, PVR, games/RetroPlayer.
- 60 fps video, 4K remux (> 50 Mb/s), AV1, VC-1, MPEG-2, TrueHD/Atmos, DTS:X.
- Launching through the hbmenu NetLoader (nxlink). So far every run started from the hbmenu file list.
