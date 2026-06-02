#!/bin/sh
# aml-display.sh — prepare/restore the Amlogic display layer around ffmpeg.
#
# WHY THIS EXISTS
#   The h264_aml/etc. decoder sets the *decode* pipeline (vfm map, tsync,
#   freerun, disable_video, blackout_policy). It deliberately does NOT switch
#   HDMI resolution or manage the OSD/console — that is display/output policy
#   and belongs here, not inside a libavcodec decoder. Kodi does this same
#   setup on start, which is why ffmpeg may only display after kodi.bin has run.
#
# USAGE
#   ./aml-display.sh start [MODE]      # prep display for ffmpeg, optionally at
#                                       # a resolution preset (480p|720p|1080p)
#   ./aml-display.sh stop              # restore (console back, video blanked)
#   ./aml-display.sh run [-r MODE] <ffmpeg args>
#                                       # start [MODE] -> ffmpeg <args> -> stop
#
# Resolution presets set both /sys/class/display/mode and the fb0 geometry
# (the double-height vyres reserves a second page for the video plane to
# composite into). Override the default with MODE=720p or the [MODE] argument.
#
# NOTE: on some MX firmwares Kodi's prep is framebuffer/ioctl-based; confirm on a
# fresh boot whether a bare ffmpeg already displays before relying on this.

FFMPEG=${FFMPEG:-/storage/ffmpeg/ffmpeg}
MODE=${MODE:-1080p}         # 480p | 720p | 1080p (see /sys/class/display/mode)

set_node() { [ -e "$1" ] && echo "$2" > "$1" 2>/dev/null; }

# set HDMI mode + framebuffer geometry for the given resolution preset
set_mode() {
  case "$1" in
    1080p) set_node /sys/class/display/mode 1080p60hz
           fbset -fb /dev/fb0 -xres 1920 -yres 1080 -vxres 1920 -vyres 2160 -depth 32 ;;
    720p)  set_node /sys/class/display/mode 720p60hz
           fbset -fb /dev/fb0 -xres 1280 -yres 720  -vxres 1280 -vyres 1440 -depth 32 ;;
    480p)  set_node /sys/class/display/mode 480p60hz
           fbset -fb /dev/fb0 -xres 720  -yres 480  -vxres 720  -vyres 960  -depth 32 ;;
    *)     echo "unknown mode '$1' (use 480p|720p|1080p)" >&2; return 1 ;;
  esac
  echo "Display mode set to $1"
}

start() {
  set_mode "${1:-$MODE}" || exit 1
  set_node /sys/class/video/disable_video 0
  set_node /sys/class/video/blackout_policy 0
  set_node /sys/class/graphics/fb0/free_scale 0
  set_node /sys/class/ppmgr/ppscaler      0
  set_node /sys/class/graphics/fb0/blank  1   # hide the text console overlay
}

stop() {
  set_node /sys/class/video/disable_video 1   # blank the video plane
  set_node /sys/class/graphics/fb0/blank  0   # restore the console
}

case "$1" in
  start)   shift; start "$1" ;;
  stop)    stop ;;
  run)     shift
           rmode=$MODE
           [ "$1" = "-r" ] && { rmode=$2; shift 2; }
           start "$rmode"; "$FFMPEG" "$@"; rc=$?; stop; exit $rc ;;
  *) echo "usage: $0 {start [MODE]|stop|run [-r MODE] <ffmpeg args>}   (MODE: 480p|720p|1080p)"; exit 1 ;;
esac
