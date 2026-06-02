# Building ffmpeg-aml for the AML8726-MX

Cross-compile on an x86_64 Linux host for the ARMv7 Cortex-A9 device. The
golden rule: **build with a toolchain whose glibc is ≤ the device's (2.29).**
We use Bootlin's prebuilt **glibc-2.28** toolchain — this is what makes the
binary run natively on LibreELEC with no glibc-symbol patching. Do **not** use
Debian's `gcc-arm-linux-gnueabihf` (glibc 2.36+); it produces symbols the device
lacks.

| Item | Value |
|------|-------|
| Host | x86_64 Linux (Debian/Ubuntu tested) |
| Toolchain | Bootlin `armv7-eabihf--glibc--bleeding-edge-2018.11-1` (gcc 8.2, glibc 2.28) |
| Triplet | `arm-buildroot-linux-gnueabihf-` |
| Target | AML8726-MX, fragtion's LibreELEC-Amlogic_MX2.arm-9.2.8.16, kernel 3.10, glibc 2.29 |
| Static deps | zlib 1.3.2, OpenSSL 4.0.0, x264 (git), libsrt (git), alsa-lib 1.2.16 |
| Dynamic (device) | libamcodec.so, glibc, libstdc++ |

Target image / `libamcodec.so` source:
https://forum.libreelec.tv/thread/4858-9-0-0-libreelec-builds-for-mx2-g18/?postID=198133#post198133

The whole build runs as five phases: install host packages (0), unpack the
toolchain (1), set the cross environment (2), build the static dependencies (3),
stage the amcodec headers + link stub (4), then fetch FFmpeg, apply the overlay,
configure, build and deploy (5–7).

## 0. Host packages

On a Debian/Ubuntu host:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential git wget ca-certificates \
  lbzip2 xz-utils cmake pkg-config python3
```

What each is for: `build-essential` (host gcc/make), `git` (FFmpeg, x264, srt
checkouts), `wget`/`ca-certificates` (downloads over HTTPS), `lbzip2`/`xz-utils`
(unpack the `.tar.bz2`/`.tar.xz` tarballs), `cmake` (builds libsrt),
`pkg-config` (dependency detection; we wrap it for cross use below), `python3`
(runs the edits in `integrate.sh`).

## 1. Toolchain

```sh
cd /opt
wget https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/armv7-eabihf--glibc--bleeding-edge-2018.11-1.tar.bz2
tar xf armv7-eabihf--glibc--bleeding-edge-2018.11-1.tar.bz2
cd armv7-eabihf--glibc--bleeding-edge-2018.11-1 && ./relocate-sdk.sh
```

## 2. Environment

```sh
cat > ~/cross-env.sh <<'EOF'
TC=/opt/armv7-eabihf--glibc--bleeding-edge-2018.11-1
export PATH=$TC/bin:$PATH
export CROSS=arm-buildroot-linux-gnueabihf
export CROSS_PREFIX=${CROSS}-
export PREFIX=/opt/armhf-glibc228
export SYSROOT=$($TC/bin/${CROSS}-gcc --print-sysroot)
export CC=${CROSS_PREFIX}gcc CXX=${CROSS_PREFIX}g++ AR=${CROSS_PREFIX}ar
export RANLIB=${CROSS_PREFIX}ranlib STRIP=${CROSS_PREFIX}strip NM=${CROSS_PREFIX}nm LD=${CROSS_PREFIX}ld
# do NOT export AS (breaks x264 .S assembly)
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="${PREFIX}"
export CFLAGS="-march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard -mthumb-interwork -O2 -pipe"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-L${PREFIX}/lib"
EOF
source ~/cross-env.sh
mkdir -p $PREFIX/{lib,include,bin}

# a pkg-config that only ever sees our cross prefix
sudo tee /usr/local/bin/${CROSS}-pkg-config >/dev/null <<WRAP
#!/bin/sh
PKG_CONFIG_PATH=${PREFIX}/lib/pkgconfig PKG_CONFIG_LIBDIR=${PREFIX}/lib/pkgconfig \
PKG_CONFIG_SYSROOT_DIR=${PREFIX} exec pkg-config "\$@"
WRAP
sudo chmod +x /usr/local/bin/${CROSS}-pkg-config
```

## 3. Static dependencies

Run each block in order; all install into `${PREFIX}`.

```sh
source ~/cross-env.sh

# zlib
cd /tmp && wget https://zlib.net/zlib-1.3.2.tar.gz && tar xf zlib-1.3.2.tar.gz && cd zlib-1.3.2
CHOST=${CROSS} ./configure --prefix=${PREFIX} --static && make -j$(nproc) && make install

# OpenSSL (unset tool vars so it builds names from --cross-compile-prefix)
cd /tmp && wget https://github.com/openssl/openssl/releases/download/openssl-4.0.0/openssl-4.0.0.tar.gz
tar xf openssl-4.0.0.tar.gz && cd openssl-4.0.0
( unset CC CXX AR AS LD RANLIB
  ./Configure linux-armv4 --cross-compile-prefix=${CROSS}- --prefix=${PREFIX} \
    --openssldir=${PREFIX}/ssl no-shared no-tests no-ui-console \
    -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard
  make -j$(nproc) && make install_sw )

# x264
cd /tmp && git clone --depth 1 https://code.videolan.org/videolan/x264.git && cd x264
./configure --prefix=${PREFIX} --host=${CROSS} --cross-prefix=${CROSS_PREFIX} \
  --enable-static --disable-opencl --disable-cli --enable-pic \
  --extra-cflags="${CFLAGS}"
make -j$(nproc) && make install

# libsrt
cd /tmp && git clone --depth 1 https://github.com/Haivision/srt.git && cd srt && mkdir build && cd build
cmake .. -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
  -DCMAKE_C_COMPILER=${CROSS}-gcc -DCMAKE_CXX_COMPILER=${CROSS}-g++ \
  -DCMAKE_FIND_ROOT_PATH=${PREFIX} -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_INSTALL_PREFIX=${PREFIX} -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_APPS=OFF \
  -DUSE_OPENSSL_PC=OFF -DOPENSSL_ROOT_DIR=${PREFIX} -DOPENSSL_INCLUDE_DIR=${PREFIX}/include \
  -DOPENSSL_SSL_LIBRARY=${PREFIX}/lib/libssl.a -DOPENSSL_CRYPTO_LIBRARY=${PREFIX}/lib/libcrypto.a \
  -DCMAKE_C_FLAGS="${CFLAGS}" -DCMAKE_CXX_FLAGS="${CFLAGS}"
make -j$(nproc) && make install
# srt.pc Requires.private breaks cross pkg-config:
sed -i 's/^Requires.private: openssl libcrypto/Requires.private:/' ${PREFIX}/lib/pkgconfig/srt.pc

# alsa-lib  (--with-configdir points runtime config at the DEVICE's copy)
cd /tmp && wget https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.16.tar.bz2
tar xf alsa-lib-1.2.16.tar.bz2 && cd alsa-lib-1.2.16
./configure --host=${CROSS} --prefix=${PREFIX} --enable-static --disable-shared \
  --disable-python --without-debug --disable-topology \
  --with-configdir=/usr/share/alsa --with-plugindir=/usr/lib/alsa-lib
make -j$(nproc) && make install
# alsa-lib may install under ${PREFIX}/usr; make it visible to FFmpeg:
[ -f ${PREFIX}/usr/lib/libasound.a ] && cp ${PREFIX}/usr/lib/libasound.a ${PREFIX}/lib/
[ -d ${PREFIX}/usr/include/alsa   ] && cp -r ${PREFIX}/usr/include/alsa ${PREFIX}/include/
```

## 4. amcodec headers + link stub

```sh
source ~/cross-env.sh
mkdir -p ${PREFIX}/include/amcodec/{amports,ppmgr}
BASE="https://raw.githubusercontent.com/numbqq/libamcodec/master/amcodec/include"
for f in codec.h codec_type.h codec_error.h codec_msg.h; do wget -q "$BASE/$f" -O ${PREFIX}/include/amcodec/$f; done
for f in amstream.h vformat.h aformat.h; do wget -q "$BASE/amports/$f" -O ${PREFIX}/include/amcodec/amports/$f; done
wget -q "$BASE/ppmgr/ppmgr.h" -O ${PREFIX}/include/amcodec/ppmgr/ppmgr.h
ln -sf ${PREFIX}/include/amcodec/amports ${PREFIX}/include/amports
ln -sf ${PREFIX}/include/amcodec/ppmgr   ${PREFIX}/include/ppmgr

# Link-time stub (the real libamcodec.so comes from the device). Must declare
# every codec_* we call plus the audio_*/get_* symbols the real lib references.
cat > /tmp/amcodec_stub.c <<'CEOF'
#include <stdlib.h>
typedef void codec_para_t;
typedef struct { int data_len; int free_len; int read_pointer; int write_pointer; } buf_status;
int  codec_init(codec_para_t *p){return 0;}            int  codec_close(codec_para_t *p){return 0;}
int  codec_reset(codec_para_t *p){return 0;}           void codec_resume(codec_para_t *p){}
int  codec_write(codec_para_t *p,void*b,int l){return l;} int codec_checkin_pts(codec_para_t*p,unsigned long t){return 0;}
int  codec_get_vbuf_state(codec_para_t*p,buf_status*s){return 0;}
int  codec_set_cntl_mode(void*p,unsigned m){return 0;} int codec_set_cntl_avthresh(void*p,unsigned v){return 0;}
int  codec_set_cntl_syncthresh(void*p,unsigned v){return 0;} int codec_init_cntl(void*p){return 0;}
int  codec_poll_cntl(void*p){return 0;}                int codec_pause(void*p){return 0;}
int  codec_write_sub_data(void*p,void*b,int l){return 0;}
int  audio_decode_init(void*p){return 0;}              int audio_decode_basic_init(void){return 0;}
int  audio_decode_release(void**p){return 0;}          int audio_get_pts(void*p,unsigned long*t){return 0;}
int  get_decoder_status(void*p,void*s){return 0;}      int get_audio_decoder(void*p){return 0;}
CEOF
${CROSS}-gcc -shared -fPIC -march=armv7-a -mfpu=neon-vfpv3 -mfloat-abi=hard \
  -o ${PREFIX}/lib/libamcodec.so /tmp/amcodec_stub.c
cp ${PREFIX}/lib/libamcodec.so ${SYSROOT}/usr/lib/libamcodec.so
```

## 5. Get upstream FFmpeg + apply the overlay

Run this from the directory that contains this overlay (so `integrate.sh` and
`PINNED_COMMIT` are alongside).

```sh
cd /root/
chmod +x integrate.sh
git clone https://github.com/FFmpeg/FFmpeg.git ffmpeg
cd ffmpeg && git checkout $(awk '/^[0-9a-f]/{print $1}' ../PINNED_COMMIT | tail -1)
cd ..
./integrate.sh ./ffmpeg          # copies the decoder sources + applies the edits
```

## 6. Configure & build

```sh
source ~/cross-env.sh
cd ffmpeg
./configure \
  --enable-cross-compile --arch=arm --target-os=linux \
  --cross-prefix=${CROSS_PREFIX} --sysroot=${SYSROOT} \
  --pkg-config=${CROSS}-pkg-config --prefix=${PREFIX} \
  --extra-cflags="-I${PREFIX}/include -I${PREFIX}/include/amcodec ${CFLAGS}" \
  --extra-ldflags="-L${PREFIX}/lib" \
  --enable-gpl --enable-version3 \
  --enable-aml --enable-libx264 --enable-libsrt --enable-openssl --enable-alsa \
  --enable-network \
  --enable-protocol=tcp --enable-protocol=udp --enable-protocol=rtp \
  --enable-demuxer=rtsp --enable-demuxer=rtp --enable-muxer=rtsp \
  --enable-bsf=h264_mp4toannexb --enable-bsf=hevc_mp4toannexb \
  --disable-doc --disable-ffplay --disable-debug \
  --extra-libs="-Wl,-Bdynamic -lamcodec -Wl,-Bstatic -lsrt -lstdc++ -lx264 -lasound -lssl -lcrypto -lz -Wl,-Bdynamic -lpthread -lm -ldl"

make -j$(nproc)
${CROSS_PREFIX}strip --strip-unneeded ffmpeg

# sanity checks
./ffmpeg -decoders 2>/dev/null | grep aml
${CROSS_PREFIX}readelf -V ffmpeg | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV | tail
#   the highest GLIBC version listed must be <= GLIBC_2.28
```

> If you re-run `integrate.sh` or change any `--enable-*`, do `make distclean &&
> ./configure ...` again — `--enable-aml`/`--enable-alsa` only take effect when
> configure regenerates `ffbuild/config.mak`.

## 7. Deploy

```sh
scp ffmpeg scripts/aml-display.sh root@<device>:/storage/ffmpeg/

# on the device (libamcodec.so ships in /usr/lib on fragtion's LibreELEC,
# so no LD_LIBRARY_PATH is needed):
./aml-display.sh start 1080p
./ffmpeg -re -c:v h264_aml -i test.mp4 -an -f null -     # video only
# video + audio: add a second output mapping audio to ALSA (see README)
```

## Troubleshooting

- `version 'GLIBC_2.xx' not found` → wrong toolchain; use the glibc-2.28 one.
- `ERROR: amcodec not found` → ensure `--extra-cflags` has both `-I${PREFIX}/include`
  and `-I${PREFIX}/include/amcodec`, and the stub/headers from step 4 exist.
- `cannot find -lasound` → alsa-lib landed in `${PREFIX}/usr/...`; copy as in step 3.
- `Cannot access .../share/alsa/alsa.conf` (device) → rebuild alsa-lib with
  `--with-configdir=/usr/share/alsa`, or `export ALSA_CONFIG_PATH=/usr/share/alsa/alsa.conf`.
- `arm-...-as: invalid option -- 'S'` (x264) → `AS` was exported; unset it.
- `tar: ... : Cannot open: No such file` on `.tar.bz2` → install `lbzip2` (or `bzip2`).
- Display stays black → run `scripts/aml-display.sh start <mode>` (HDMI/framebuffer
  setup is separate from decode).
