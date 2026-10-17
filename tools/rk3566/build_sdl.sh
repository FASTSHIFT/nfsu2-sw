#!/usr/bin/env bash
# SDL 2.0.10 (Ubuntu focal's source) with KMSDRM + ALSA + libudev, cross-built for
# aarch64/glibc 2.31 in the ug2-r36s-cross:focal image (tools/ug2/Dockerfile).
# Prepare /tmp/sdlbuild first: fetch_sdl_deps.sh (arm64 -dev debs -> debs/) and the
# unpacked libsdl2_2.0.10+dfsg1.orig.tar.xz in src/ (xz is not in the image) with
# sdl-kmsdrm-mode.patch applied. Output: out/lib/libSDL2-2.0.so.0.10.0.
# KMSDRM links libdrm.so.2/libgbm.so.1 directly (run with the Mali libgbm first on
# LD_LIBRARY_PATH); udev is dlopen'ed (needed for evdev keyboards); ALSA is linked.
set -euo pipefail
cd /tmp/sdlbuild
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v /tmp/sdlbuild:/w -w /w ug2-r36s-cross:focal bash -c '
set -euo pipefail
# headers from the arm64 -dev debs (unpack to a staging prefix, no root needed)
rm -rf stage out b; mkdir -p stage out b
for d in debs/*.deb; do dpkg-deb -x "$d" stage; done
mkdir -p stage/usr/include/X11
printf "typedef struct _XDisplay Display;\ntypedef unsigned long XID;\ntypedef XID Window;\ntypedef XID Pixmap;\ntypedef XID Colormap;\n" > stage/usr/include/X11/Xlib.h
printf "" > stage/usr/include/X11/Xutil.h
mkdir -p b && cd b
S=/w/stage
export PKG_CONFIG_LIBDIR=$S/usr/lib/aarch64-linux-gnu/pkgconfig:$S/usr/share/pkgconfig PKG_CONFIG_SYSROOT_DIR=$S
../src/configure --host=aarch64-linux-gnu --prefix=/w/out \
  CFLAGS="-O2 -DMESA_EGL_NO_X11_HEADERS -I$S/usr/include -I$S/usr/include/libdrm -I$S/usr/include/aarch64-linux-gnu" \
  LDFLAGS="-L$S/usr/lib/aarch64-linux-gnu -L$S/lib/aarch64-linux-gnu" \
  --disable-video-x11 --disable-video-wayland --disable-video-opengl --enable-video-opengles \
  --enable-video-kmsdrm --enable-kmsdrm-shared --enable-alsa --disable-alsa-shared \
  --disable-pulseaudio --disable-esd --disable-jack --disable-sndio --disable-arts --disable-nas \
  --disable-video-vulkan --disable-video-rpi --disable-video-vivante --disable-video-cocoa \
  --disable-dbus --disable-ibus --disable-fcitx --disable-diskaudio --disable-sensor \
  > ../configure.log 2>&1 || { tail -30 ../configure.log; exit 1; }
grep -iE "kmsdrm|alsa|gbm|drm" ../configure.log | tail -8
make -j$(nproc) > ../make.log 2>&1 || { tail -30 ../make.log; exit 1; }
make install > ../install.log 2>&1
ls -la /w/out/lib | head
'
