# Reproducible firmware/host-test toolchain. See doc/folder-cards.md.
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake gcc-arm-none-eabi git \
    libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib make python3 python3-venv
WORKDIR /src
