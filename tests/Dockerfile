# SPDX-License-Identifier: MPL-2.0
# Noble's community Mbed TLS 2.28.8 is affected by CVE-2025-27810.
FROM ubuntu:26.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential clang libclang-rt-dev git pkg-config python3 perl ca-certificates \
    libarchive-dev libext2fs-dev comerr-dev e2fsprogs libmbedtls-dev \
    && rm -rf /var/lib/apt/lists/*
