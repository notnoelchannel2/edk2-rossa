#!/bin/bash
# based on the instructions from edk2-platforms
# usage: ./build.sh [device] [DEBUG|RELEASE]   (device: j5nlte, j5xlte, rossa)
DEVICE=${1:-j5nlte}
TARGET=${2:-DEBUG}
rm -rf "boot-$DEVICE.img"
set -e
export PACKAGES_PATH=$PWD/edk2:$PWD/edk2-platforms:$PWD
export WORKSPACE=$PWD/workspace
# edksetup.sh parses "$@" when sourced, so don't leak our arguments into it
set --
. edk2/edksetup.sh

# not actually GCC5; it's GCC7 on Ubuntu 18.04.
# anyways, start the build
GCC5_AARCH64_PREFIX=aarch64-linux-gnu- build -s -n 0 -a AARCH64 -t GCC5 -b "$TARGET" -p "Msm8916Pkg/Devices/$DEVICE.dsc" $BUILD_EXTRA

# post-build packing
gzip -c < "workspace/Build/Msm8916Pkg/${TARGET}_GCC5/FV/MSM8916PKG_UEFI.fd" > "boot-$DEVICE.img"||exit 1
cat "ImageResources/$DEVICE.dtb" >>"boot-$DEVICE.img"||exit 1
