#!/usr/bin/env bash
#
# integrate.sh — overlay the Amlogic AML8726-MX hardware decoder onto an
# FFmpeg source tree.
#
#   usage: ./integrate.sh /path/to/ffmpeg
#
# Copies the 7 new decoder source files into libavcodec/ and applies the
# 5 edits to existing upstream files (configure, libavcodec/Makefile,
# libavcodec/allcodecs.c, libavutil/pixfmt.h, libavutil/pixdesc.c).
#
# Edits are anchored on upstream strings and are IDEMPOTENT — re-running is
# safe, and they tolerate upstream line-number drift. Tested against the
# pinned commit in PINNED_COMMIT, but should apply to nearby revisions.

set -euo pipefail

FF="${1:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ -z "$FF" ] || [ ! -f "$FF/libavcodec/allcodecs.c" ]; then
  echo "usage: $0 /path/to/ffmpeg   (must be an FFmpeg source tree)" >&2
  exit 1
fi

echo ">> copying decoder sources into libavcodec/"
cp "$HERE"/src/libavcodec/amldec.c   "$FF/libavcodec/"
cp "$HERE"/src/libavcodec/amldec.h   "$FF/libavcodec/"
cp "$HERE"/src/libavcodec/amltools.c "$FF/libavcodec/"
cp "$HERE"/src/libavcodec/amltools.h "$FF/libavcodec/"
cp "$HERE"/src/libavcodec/amlqueue.c "$FF/libavcodec/"
cp "$HERE"/src/libavcodec/amlqueue.h "$FF/libavcodec/"
cp "$HERE"/src/libavcodec/aml.h      "$FF/libavcodec/"

echo ">> applying edits to existing files (idempotent)"
FF="$FF" python3 - <<'PYEOF'
import os, sys
FF = os.environ["FF"]

def edit(relpath, marker, anchor, insert_after=True, replace=None):
    """Insert text relative to an anchor, unless marker already present.
    If replace is given, replace `anchor` with `replace` instead of inserting."""
    path = os.path.join(FF, relpath)
    with open(path) as f:
        txt = f.read()
    if marker in txt:
        print(f"   = {relpath}: already patched")
        return
    if anchor not in txt:
        sys.exit(f"   ! {relpath}: anchor not found:\n     {anchor!r}\n"
                 f"     upstream layout changed — patch manually.")
    if replace is not None:
        txt = txt.replace(anchor, replace, 1)
    elif insert_after:
        txt = txt.replace(anchor, anchor + insert, 1)
    else:
        txt = txt.replace(anchor, insert + anchor, 1)
    with open(path, "w") as f:
        f.write(txt)
    print(f"   + {relpath}: patched")

# 1) libavutil/pixfmt.h — add AV_PIX_FMT_AML after AV_PIX_FMT_MMAL
insert = (
"\n    /**\n"
"     * HW acceleration through Amlogic amcodec, data[0] contains a pointer\n"
"     * to the codec_para_t structure.\n"
"     */\n"
"    AV_PIX_FMT_AML,\n"
)
edit("libavutil/pixfmt.h", "AV_PIX_FMT_AML", "    AV_PIX_FMT_MMAL,\n")

# 2) libavutil/pixdesc.c — add the AML descriptor after the MMAL descriptor
insert = (
'    [AV_PIX_FMT_AML] = {\n'
'        .name = "aml",\n'
'        .flags = AV_PIX_FMT_FLAG_HWACCEL,\n'
'    },\n'
)
edit("libavutil/pixdesc.c", "[AV_PIX_FMT_AML]",
     '    [AV_PIX_FMT_MMAL] = {\n        .name = "mmal",\n        .flags = AV_PIX_FMT_FLAG_HWACCEL,\n    },\n')

# 3) libavcodec/allcodecs.c — extern decls (configure derives the codec list
#    by grepping this file, so this is what makes the decoders "exist")
insert = (
"extern const FFCodec ff_h264_aml_decoder;\n"
"extern const FFCodec ff_hevc_aml_decoder;\n"
"extern const FFCodec ff_mpeg2_aml_decoder;\n"
"extern const FFCodec ff_mpeg4_aml_decoder;\n"
"extern const FFCodec ff_msmpeg4v1_aml_decoder;\n"
"extern const FFCodec ff_msmpeg4v2_aml_decoder;\n"
"extern const FFCodec ff_msmpeg4v3_aml_decoder;\n"
"extern const FFCodec ff_vc1_aml_decoder;\n"
"extern const FFCodec ff_mjpeg_aml_decoder;\n"
"extern const FFCodec ff_rv30_aml_decoder;\n"
"extern const FFCodec ff_rv40_aml_decoder;\n"
)
edit("libavcodec/allcodecs.c", "ff_h264_aml_decoder",
     "extern const FFCodec ff_h264_mmal_decoder;\n")

# 4) libavcodec/Makefile — object rules
insert = (
"OBJS-$(CONFIG_H264_AML_DECODER)        += amldec.o amltools.o amlqueue.o\n"
"OBJS-$(CONFIG_HEVC_AML_DECODER)        += amldec.o amltools.o amlqueue.o\n"
"OBJS-$(CONFIG_MPEG2_AML_DECODER)       += amldec.o amltools.o amlqueue.o\n"
"OBJS-$(CONFIG_MPEG4_AML_DECODER)       += amldec.o amltools.o amlqueue.o\n"
"OBJS-$(CONFIG_MSMPEG4V1_AML_DECODER)   += amldec.o amltools.o amlqueue.o\n"
"OBJS-$(CONFIG_MSMPEG4V2_AML_DECODER)   += amldec.o amltools.o amlqueue.o\n"
"OBJS-$(CONFIG_MSMPEG4V3_AML_DECODER)   += amldec.o amltools.o amlqueue.o\n"
"OBJS-$(CONFIG_VC1_AML_DECODER)         += amldec.o amltools.o amlqueue.o\n"
"OBJS-$(CONFIG_MJPEG_AML_DECODER)       += amldec.o amltools.o amlqueue.o\n"
"OBJS-$(CONFIG_RV30_AML_DECODER)        += amldec.o amltools.o amlqueue.o\n"
"OBJS-$(CONFIG_RV40_AML_DECODER)        += amldec.o amltools.o amlqueue.o\n"
)
edit("libavcodec/Makefile", "CONFIG_H264_AML_DECODER",
     "OBJS-$(CONFIG_H264_NVENC_ENCODER)", insert_after=False)

# 5a) configure — help text
insert = "  --enable-aml             enable decoding via Amlogic HW decoders [no]\n"
edit("configure", "--enable-aml",
     "  --enable-omx-rpi         enable OpenMAX IL code for Raspberry Pi [no]\n")

# 5b) configure — HWACCEL_LIBRARY_LIST
edit("configure", "\n    aml\n    mmal\n",
     "    mmal\n    omx\n    opencl\n",
     replace="    aml\n    mmal\n    omx\n    opencl\n")

# 5c) configure — decoder deps
insert = (
'h264_aml_decoder_deps="aml"\n'
'hevc_aml_decoder_deps="aml"\n'
'mpeg2_aml_decoder_deps="aml"\n'
'mpeg4_aml_decoder_deps="aml"\n'
'msmpeg4v1_aml_decoder_deps="aml"\n'
'msmpeg4v2_aml_decoder_deps="aml"\n'
'msmpeg4v3_aml_decoder_deps="aml"\n'
'vc1_aml_decoder_deps="aml"\n'
'mjpeg_aml_decoder_deps="aml"\n'
'rv30_aml_decoder_deps="aml"\n'
'rv40_aml_decoder_deps="aml"\n'
)
edit("configure", 'h264_aml_decoder_deps',
     'h264_nvenc_encoder_deps="nvenc"\n', insert_after=False)

# 5d) configure — amcodec library detection. Relies on --extra-cflags carrying
#     -I<prefix>/include and -I<prefix>/include/amcodec (see BUILD.md).
insert = (
'enabled aml               && { check_lib aml amcodec/codec.h codec_init -lamcodec ||\n'
'                               die "ERROR: amcodec not found (ensure --extra-cflags has -I<prefix>/include and -I<prefix>/include/amcodec)"; }\n'
)
edit("configure", "enabled aml               &&",
     "enabled mmal              && { check_lib mmal", insert_after=False)

print(">> done")
PYEOF

echo ">> integration complete. Now configure & build (see BUILD.md)."
