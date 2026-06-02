# ffmpeg-aml — Amlogic AML8726-MX hardware video decoder for FFmpeg

An overlay that adds Amlogic `amcodec` hardware **decoding** to a modern FFmpeg
(libavcodec 62.x / FFmpeg 8.x). It targets the **AML8726-MX (MX2)** ARMv7
Cortex-A9 SoC running LibreELEC (kernel 3.10), driving video straight to the
HDMI display layer via the kernel `amvideo`/`vfm` pipeline.

This is an **overlay**, not a fork: it ships the decoder source files plus an
`integrate.sh` that applies them to a clean upstream FFmpeg checkout pinned to a
known-good commit (see `PINNED_COMMIT`). That keeps the change small and
reviewable, preserves clean attribution, and avoids carrying a multi-thousand-
commit fork.

## Lineage / credit

Originally derived from LongChair's `amlvideo` branch of FFmpeg (~2016, FFmpeg 3.x).
That code has been substantially ported to the modern decoder API and extended.
Credit also to codesnake for bringing LibreElec/Kodi support to this platform - 
The amcodec usage patterns follow Kodi's `AMLCodec.cpp`/`AMLUtils.cpp`. 
Original copyright headers are preserved in the source files.

## Target device

Built and tested against **fragtion's LibreELEC-Amlogic_MX2.arm-9.2.8.16**
image (kernel 3.10, glibc 2.29). See the LibreELEC forum thread:
https://forum.libreelec.tv/thread/4858-9-0-0-libreelec-builds-for-mx2-g18/?postID=198133#post198133

The real `libamcodec.so` is provided by that LibreELEC image and is linked
dynamically at runtime; a link-time stub is used when cross-compiling (see
BUILD.md).

## Quick start

Build (cross-compile on an x86_64 Linux host — see **BUILD.md** for the full
toolchain and dependency setup):

```sh
git clone https://github.com/FFmpeg/FFmpeg.git ffmpeg
cd ffmpeg && git checkout 49a77d37be      # the pinned commit; see PINNED_COMMIT
cd ..
./integrate.sh ./ffmpeg                    # copies sources + applies edits
# then configure & build per BUILD.md
```

Deploy and run on the device:

```sh
scp ffmpeg scripts/aml-display.sh root@<device>:/storage/ffmpeg/

# on the device:
./aml-display.sh start 1080p               # set HDMI mode + framebuffer first
./ffmpeg -re -c:v h264_aml -i input.mp4 -an -f null -
```

(`libamcodec.so` ships in `/usr/lib` on fragtion's LibreELEC image, so no
`LD_LIBRARY_PATH` is needed.)

## Supported codecs

Confirmed working on AML8726-MX (verified against
`/sys/class/amstream/vcodec_profile` and on-device playback):

- `h264_aml` — H.264 / AVC
- `mpeg2_aml` — MPEG-1/2
- `mpeg4_aml` — MPEG-4 ASP (Xvid / DivX 4–6)
- `vc1_aml` — VC-1 / WMV3
- `mjpeg_aml` — Motion JPEG

Registered but not usable on MX firmware (they may work on newer Amlogic SoCs
or other firmwares):

- `rv30_aml` — RealVideo 8 / RV30 (experimental)
- `rv40_aml` — RealVideo 9 / RV40 / RMVB (`codec_init` fails on this firmware)
- `msmpeg4v1_aml` / `msmpeg4v2_aml` / `msmpeg4v3_aml` — MS-MPEG4 / DivX 3.11
  (no DivX-3 microcode in this firmware)
- `hevc_aml` — H.265 / HEVC (MX silicon predates HEVC)

## Behavior — read before using

- **Direct-render, display-only.** The VPU decodes and renders the picture
  directly to HDMI. FFmpeg receives only a lightweight blank placeholder frame
  per packet (enough to keep its output pipeline running); the real picture
  never comes back to FFmpeg. This is for *playback on the device*, not
  transcoding.
- **No software output.** `-i ... out.mp4` captures only the blank placeholders,
  not the decoded picture — it exists only on the hardware overlay. There is no
  `hwdownload` path.
- **CLI output.** The `frame=` counter advances (those are the placeholders).
  Containers without a stored pixel format (TS/AVI/ASF) may print *"unspecified
  pixel format"* and exit non-zero — the video still plays on HDMI.
- **Pace file inputs with `-re`.** Live sources (RTSP/SRT) are already realtime.

## Example usage

Video only (no sound):

```sh
./ffmpeg -re -c:v h264_aml -i input.mp4 -an -f null -
```

Video + audio (audio decoded to ALSA):

```sh
./ffmpeg -re -c:v h264_aml -i input.mp4 \
    -map 0:v:0 -an -f null - \
    -map 0:a:0 -vn -ac 2 -c:a pcm_s16le -f alsa hw:0,0
```

The first output sends video to the hardware decoder (no frames come back, so
`-f null`); the second decodes audio to ALSA. Use `-ac 2` (stereo) or the pitch
doubles, and point `hw:0,0` at your ALSA device. The ALSA output paces itself
against the sound card, keeping audio and video roughly aligned (independent A/V
output, not frame-accurate lip sync).

For a live source, drop `-re` (RTSP/SRT are already realtime):

```sh
./ffmpeg -c:v h264_aml -i rtsp://camera/stream -an -f null -
```

## Display setup

The decoder configures the decode pipeline but not the HDMI output or the
OSD/console — that is display policy. `scripts/aml-display.sh` handles it,
including setting the output resolution (HDMI mode + framebuffer geometry). On a
fresh boot the video plane may stay blank until this has run (Kodi normally does
the equivalent on start).

```sh
./scripts/aml-display.sh start 1080p     # prep display at 1080p (or 720p / 480p)
./scripts/aml-display.sh stop            # restore console, blank the video plane
./scripts/aml-display.sh run -r 720p -c:v h264_aml -i input.mp4 -an -f null -
                                         # start -> ffmpeg -> stop, in one go
```

`start` defaults to 1080p (override with the argument or `MODE=720p`). If a bare
ffmpeg already displays on your firmware you may not need this.

## License

The decoder files are LGPL-2.1-or-later, consistent with FFmpeg and the original
LongChair code. The full build (with `--enable-gpl --enable-libx264`) produces a
GPL binary. See [`LICENSE`](LICENSE) for details.

---

## Contributing

Pull requests, forks, issues and suggestions are all welcome.

---

## Support

If this project has been useful to you, consider buying me a coffee:

**PayPal:** [![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://www.paypal.com/donate/?business=2CGE77L7BZS3S&no_recurring=0)  
**BTC:** `1Q4QkBn2Rx4hxFBgHEwRJXYHJjtfusnYfy`  
**XMR:** `4AfeGxGR4JqDxwVGWPTZHtX5QnQ3dTzwzMWLBFvysa6FTpTbz8Juqs25XuysVfowQoSYGdMESqnvrEQ969nR9Q7mEgpA5Zm`