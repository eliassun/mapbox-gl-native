#!/usr/bin/env bash

set -e
set -o pipefail

source ./scripts/travis_helper.sh

mapbox_time "checkout_mason" \
git submodule update --init .mason

export MASON_PLATFORM=android

for ABI in ${ANDROID_ABIS:-arm-v7} ; do
    export ANDROID_ABI=${ABI}
    export MASON_ANDROID_ABI=${ANDROID_ABI}
    mapbox_time "android_toolchain" \
    ./scripts/android/toolchain.sh
done
