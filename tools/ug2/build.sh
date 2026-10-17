#!/usr/bin/env bash
# 交叉编译 OpenUG2 (`make gles`) 到 R36S aarch64。
#
#   sg docker -c 'bash ug2-r36s/build.sh'
#
# 上游 Makefile 把所有 .c 放在一条 cc 命令里编译(单线程)，这里改成逐文件并行编译再链接，
# 编译参数与 Makefile 的 `gles` 目标保持一致，另加 -mcpu=cortex-a35。
# 在 build/src 的副本里编译，不动 OpenUG2 目录里的 PC 构建产物。
# 输出: ug2-r36s/build/nfsu2
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC_REPO="$(realpath "${UG2_SRC:-$HERE/../OpenUG2}")"
OUT="$HERE/build"
mkdir -p "$OUT/src"
# 只同步源码和 shader(含 PC 构建生成的 generated_*.h, 容器里没有 xxd)
rsync -a --delete --exclude .git --exclude build --exclude nfsu2 "$SRC_REPO/" "$OUT/src/"
test -f "$OUT/src/src/generated_shaders.h" || { echo "缺 generated_shaders.h: 先在 OpenUG2 里 make 一次" >&2; exit 1; }

docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e JOBS="${JOBS:-$(nproc)}" \
    -v "$OUT/src":/src -w /src ug2-r36s-cross:focal bash -c '
set -euo pipefail
export PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig
CC=aarch64-linux-gnu-gcc
CF="-O2 -std=c99 -Wall -Wextra -Wno-unused-function -D_DEFAULT_SOURCE -DN2_GLES -mcpu=cortex-a35 \
    $(sdl2-config --cflags) $(pkg-config --cflags libpng libcrypto)"
# 源文件列表直接问 make(含 SRC += 追加项)，避免手抄漂移
SRCS=$(make -s -f Makefile -f - print-src <<<"print-src: ; @echo \$(SRC)" 2>/dev/null)
[ -n "$SRCS" ] || { echo "取不到 SRC 列表" >&2; exit 1; }
mkdir -p obj
echo "$SRCS" | tr " " "\n" | xargs -P "$JOBS" -I{} sh -c "
    o=obj/\$(echo {} | tr / _ | sed s/\\.c\$/.o/); $CC $CF -c {} -o \$o" 
# 设备(Debian 13)只有 libcrypto.so.3，没有 focal 的 so.1.1；只用到 SHA256，所以静态链接 libcrypto.a
LIBS="$(sdl2-config --libs) -lGLESv2 $(pkg-config --libs libpng) -Wl,-Bstatic -lcrypto -Wl,-Bdynamic -ldl -lpthread -lz -lm"
$CC obj/*.o -o nfsu2 $LIBS
aarch64-linux-gnu-readelf -d nfsu2 | grep NEEDED
file nfsu2
'
mkdir -p "$OUT" && cp "$OUT/src/nfsu2" "$OUT/nfsu2"
ls -la "$OUT/nfsu2"
