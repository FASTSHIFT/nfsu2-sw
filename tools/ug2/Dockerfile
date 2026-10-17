# OpenUG2 -> R36S 交叉编译镜像：在 nfsu2-r36s-cross:focal（x86_64 原生交叉 gcc 9 /
# glibc 2.31 / SDL2 arm64，不是 qemu）上叠加 OpenUG2 `make gles` 需要的 arm64
# zlib、libpng、libssl(libcrypto)、GLESv2 的运行库和 -dev 内容。
#
# 容器内 apt 下载索引极慢，所以 deb 由宿主机先下好（ug2-r36s/fetch-debs.sh），
# 这里只解包，不跑 apt。注意：镜像是 usr-merge（/lib -> usr/lib），deb 里有 ./lib/，
# 直接 `dpkg-deb -x ... /` 会把 /lib 软链接换成真目录并弄丢 x86 的 libc，
# 所以先解到暂存目录，再用 tar --keep-directory-symlink 合并。
#
#   bash ug2-r36s/fetch-debs.sh
#   docker build -t ug2-r36s-cross:focal -f ug2-r36s/Dockerfile ug2-r36s
ARG BASE=nfsu2-r36s-cross:focal
FROM ${BASE}
COPY debs/*.deb /tmp/debs/
RUN set -e; mkdir /tmp/stage; \
    for d in /tmp/debs/*.deb; do dpkg-deb -x "$d" /tmp/stage; done; \
    tar -C /tmp/stage -cf - . | tar -C / -xf - --keep-directory-symlink; \
    rm -rf /tmp/debs /tmp/stage
