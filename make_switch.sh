#!/bin/sh
rm -f CMakeCache.txt
rm -rf build
cmake -G "Unix Makefiles" --toolchain=DevkitA64Libnx.cmake -S . -B build