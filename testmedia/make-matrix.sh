#!/usr/bin/env bash
# Kodi Switch 2 playback test matrix: synthetic clips only (ffmpeg testsrc2 pattern with burnt-in
# timecode/frame counter + 440 Hz sine). 12 s each, MKV. Usage: bash make-matrix.sh [outdir]
set -euo pipefail
OUT=${1:-$(dirname "$0")/matrix}; mkdir -p "$OUT"
D=12
HDR='-pix_fmt yuv420p10le -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc'
HDRX='hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400'
A20='-f lavfi -i sine=frequency=440:sample_rate=48000'
mk() { # name size vcodec-args audio-args [extra]
  local name=$1 size=$2 vargs=$3 aargs=$4
  [ -f "$OUT/$name.mkv" ] && { echo "skip $name"; return; }
  ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=$size:rate=30" $A20 -t $D \
    $vargs $aargs -shortest "$OUT/$name.mkv"
  echo "made $name $(stat -c %s "$OUT/$name.mkv")"
}
H264='-c:v libx264 -preset veryfast -profile:v high -pix_fmt yuv420p'
HEVC='-c:v libx265 -preset ultrafast -pix_fmt yuv420p -x265-params log-level=error'
HEVC10="-c:v libx265 -preset ultrafast $HDR -x265-params log-level=error:$HDRX"
PCM2='-c:a pcm_s16le -ac 2'
AC3_51='-af pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0 -c:a ac3 -b:a 448k'
DTS_51='-af pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0 -c:a dca -strict -2 -b:a 1509k'
mk 01_720p_h264_pcm2.0          1280x720  "$H264 -b:v 4M"   "$PCM2"
mk 02_720p_hevc_ac3-5.1         1280x720  "$HEVC -b:v 3M"   "$AC3_51"
mk 03_1080p_h264_ac3-5.1        1920x1080 "$H264 -b:v 8M"   "$AC3_51"
mk 04_1080p_h264_dts-5.1        1920x1080 "$H264 -b:v 8M"   "$DTS_51"
mk 05_1080p_hevc_pcm2.0         1920x1080 "$HEVC -b:v 6M"   "$PCM2"
mk 06_1080p_hevc_dts-5.1        1920x1080 "$HEVC -b:v 6M"   "$DTS_51"
mk 07_1080p_hevc10-hdr10_ac3-5.1 1920x1080 "$HEVC10 -b:v 8M" "$AC3_51"
mk 08_2160p_h264_ac3-5.1        3840x2160 "$H264 -b:v 25M"  "$AC3_51"
mk 09_2160p_hevc_dts-5.1        3840x2160 "$HEVC -b:v 15M"  "$DTS_51"
mk 10_2160p_hevc10-hdr10_pcm2.0 3840x2160 "$HEVC10 -b:v 18M" "$PCM2"
mk 11_2160p_hevc10-hdr10_dts-5.1 3840x2160 "$HEVC10 -b:v 18M" "$DTS_51"
