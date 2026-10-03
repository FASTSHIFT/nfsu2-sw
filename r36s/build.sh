#!/usr/bin/env bash
# Build NFSU2 (Xbox recomp) for R36S / dArkOS (aarch64, glibc 2.31). Run with
# docker access:
#
#   sg docker -c 'r36s/build.sh'           # Release, native cross compiler
#   sg docker -c 'r36s/build.sh ffmpeg'    # (once) the VP6-only LGPL FFmpeg
#   sg docker -c 'QEMU=1 r36s/build.sh'    # same, inside the arm64 image (slow)
#
# Two images, same output (focal gcc 9, glibc 2.31, focal SDL 2.0.10 headers):
#   nfsu2-r36s-cross:focal    r36s/Dockerfile.cross  x86_64 + aarch64 cross gcc (default)
#   nfsu2-r36s-builder:focal  r36s/Dockerfile        arm64 under qemu (AGENT.md §1)
#
# Environment:
#   NFSU2_GEN_DIR  lifted C (default: ../NFS 8/xbox/gen)
#   JOBS           parallel jobs (default: nproc for cross, 8 under qemu; each
#                  -O2 chunk takes ~0.4 GB)
#   BUILD_DIR      default build-r36s (cross) / build-r36s-qemu
#
# Output: $BUILD_DIR/nfsu2_recomp. FFmpeg: build-r36s/ffmpeg-vp6 (from
# build/ffmpeg-7.1, the source the Linux build uses).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
GEN="$(realpath "${NFSU2_GEN_DIR:-$REPO/../NFS 8/xbox/gen}")"
CCACHE="$HOME/.cache/r36s-ccache"
if [ "${QEMU:-0}" = "1" ]; then
    IMAGE=nfsu2-r36s-builder:focal; PLATFORM=(--platform linux/arm64)
    BUILD="${BUILD_DIR:-build-r36s-qemu}"; JOBS="${JOBS:-8}"
    CC=gcc; TOOLCHAIN=""; OBJDUMP=objdump; READELF=readelf
    FF_CROSS=""
else
    IMAGE=nfsu2-r36s-cross:focal; PLATFORM=()
    BUILD="${BUILD_DIR:-build-r36s}"; JOBS="${JOBS:-$(nproc)}"
    CC=aarch64-linux-gnu-gcc
    TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=/src/r36s/toolchain-aarch64.cmake"
    OBJDUMP=aarch64-linux-gnu-objdump; READELF=aarch64-linux-gnu-readelf
    FF_CROSS="--enable-cross-compile --cross-prefix=aarch64-linux-gnu- --arch=aarch64 --target-os=linux"
fi
mkdir -p "$REPO/$BUILD" "$REPO/build-r36s" "$CCACHE"

run() {
    docker run --rm "${PLATFORM[@]}" \
        --user "$(id -u):$(id -g)" -e HOME=/tmp \
        -e CCACHE_DIR=/ccache -e CCACHE_MAXSIZE=8G -v "$CCACHE":/ccache \
        -v "$REPO":/src -v "$GEN":/gen:ro -w /src \
        "$IMAGE" bash -c "$1"
}

if [ "${1:-}" = "ffmpeg" ]; then
    [ -f "$REPO/build/ffmpeg-7.1/configure" ] || {
        echo "error: no FFmpeg source in build/ffmpeg-7.1" >&2; exit 1; }
    # build_ffmpeg_vp6.sh's linux configuration, tuned for the A35.
    run "
        set -e
        P=/src/build-r36s/ffmpeg-vp6; B=\$P-build
        rm -rf \$B && mkdir -p \$B && cd \$B
        /src/build/ffmpeg-7.1/configure --prefix=\$P $FF_CROSS \
            --disable-everything --enable-decoder=vp6 \
            --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale \
            --disable-swresample --disable-programs --disable-doc --disable-network \
            --disable-pthreads --disable-debug --disable-autodetect \
            --enable-static --disable-shared --enable-pic \
            --cc='ccache $CC' --extra-cflags=-mcpu=cortex-a35 >configure.log
        grep -E '^License' configure.log
        make -j\$(nproc) >make.log && make install >install.log
        echo installed \$P"
    exit 0
fi

[ -f "$GEN/recomp_funcs.h" ] || { echo "error: no generated code in $GEN" >&2; exit 1; }
FF=""
[ -f "$REPO/build-r36s/ffmpeg-vp6/lib/libavcodec.a" ] && FF="-DNFSU2_FFMPEG_DIR=/src/build-r36s/ffmpeg-vp6"
[ -n "$FF" ] || echo "warning: no aarch64 FFmpeg (r36s/build.sh ffmpeg) -- movies use the slow lifted decoder" >&2

# EMBEDDED: GLES renderer, no libepoxy/OpenSSL. --as-needed keeps libGL /
# libOpenGL out of NEEDED (the device only has Mali EGL/GLES; SDL loads GL at
# run time). No LTO (AGENT.md §1.4).
run "
    set -e
    cmake -S . -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release $TOOLCHAIN \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_C_FLAGS='-mcpu=cortex-a35' \
        -DCMAKE_EXE_LINKER_FLAGS=-Wl,--as-needed \
        -DXBOXRECOMP_EMBEDDED=ON -DNFSU2_GEN_DIR=/gen $FF >/dev/null
    ninja -C $BUILD -j$JOBS
    file $BUILD/nfsu2_recomp
    echo -n 'max GLIBC: '; $OBJDUMP -T $BUILD/nfsu2_recomp | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1
    $READELF -d $BUILD/nfsu2_recomp | grep NEEDED
"
