# Roadmap

State on 2026-11-25: **rev 30**. Kodi 21.3 runs as a homebrew app on Switch 2 and plays video in real time up to
2160p, including HDR10 (tone-mapped) and AV1. CPython 3.11 add-ons work, and 41 official add-ons come
pre-installed. Subtitles and audio-track switching, the web server and JSON-RPC remote, music, pictures and the
on-screen keyboard all work. The GUI follows the output size, and the screen stays on during playback. Test details
are in [TESTING.md](TESTING.md).

## Next tasks (in progress first)

1. **Pillow (`script.module.pil`)**, built statically into CPython. TMDb Helper's image features and several skins
   need it. In progress.
2. **More skins**: Arctic Horizon 2, Arctic Fuse 2 and Arctic Zephyr Mod, staged and vetted. They need Pillow for
   focus textures and blur, and internet for TMDb widgets.
3. **USB drives (read-only)**: Switch 2's USB host service works differently from Switch 1's. A mass-storage
   device can be listed; next come bulk transfers, then a read-only FAT/exFAT mount in Kodi. In progress.
4. **Internet streaming through the add-ons**: YouTube and the others with inputstream.adaptive and TLS to real
   servers. The add-on menus already work on the LAN.
5. **Hardware video decode** (NVDEC on the Switch 2 GPU). Today everything is FFmpeg software decoding on the CPU;
   hardware decode is needed for high-bitrate 4K HEVC and battery life.
6. **Docked and TV output**: 4K GUI, refresh-rate switching, HDR output if the display path allows it, and tests in
   handheld mode and across undocking.
7. **Audio passthrough and multichannel**: audout is stereo S16 at 48 kHz. Look into audren and HDMI
   multichannel when docked.
8. **Network sources**: NFS is built in but hasn't been tested against a server. SMB needs a new backend
   (libsmb2). UPnP isn't built yet.
9. **Binary add-ons**: more static registrations like inputstream.adaptive (inputstream.ffmpegdirect,
   pvr.iptvsimple). There's no `dlopen` on this platform.
10. **Long sessions**: 2-hour playback, sleep/resume (HOME and back), thermals, memory growth.
11. **Audible-audio check**: PCM, AC3, DTS, DTS-HD MA, E-AC3, FLAC and AAC 7.1 downmix to the speaker, and the
    channel mapping of the 5.1→2.0 downmix.

## Not tested yet

See the list at the end of [TESTING.md](TESTING.md).
