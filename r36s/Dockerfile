# NFSU2 (Xbox recomp) build image for R36S / dArkOS (aarch64, glibc 2.31).
# Same base as the PortMaster builder (arm64v8/ubuntu:20.04) and as
# re3-sdl/tools/r36s; adds a newer CMake (the project needs >= 3.20, focal
# ships 3.16) from pip. Base image via the daocloud mirror, apt and pip via
# aliyun (the defaults are too slow from here).
ARG BASE=docker.m.daocloud.io/arm64v8/ubuntu:20.04
FROM ${BASE}
ARG APT_MIRROR=http://mirrors.aliyun.com/ubuntu-ports
ARG PIP_INDEX=https://mirrors.aliyun.com/pypi/simple/
ENV DEBIAN_FRONTEND=noninteractive
RUN sed -i "s|http://ports.ubuntu.com/ubuntu-ports|${APT_MIRROR}|g" /etc/apt/sources.list \
 && apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build ccache git pkg-config file binutils \
        python3 python3-pip nasm make \
        libsdl2-dev libegl1-mesa-dev libgles2-mesa-dev \
 && rm -rf /var/lib/apt/lists/* \
 && pip3 install --no-cache-dir -i ${PIP_INDEX} "cmake==3.27.9"
