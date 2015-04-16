#!/usr/bin/env bash

set -e
set -o pipefail

source ./scripts/travis_helper.sh

mapbox_time "install_apt" \
sudo apt-get install mesa-utils libboost-dev libxi-dev x11proto-randr-dev \
    x11proto-xext-dev libxrandr-dev x11proto-xf86vidmode-dev libxxf86vm-dev \
    libxcursor-dev libxinerama-dev

mapbox_time "checkout_mason" \
git submodule update --init .mason
export PATH="`pwd`/.mason:${PATH}" MASON_DIR="`pwd`/.mason"

mapbox_time "install_mesa" \
mason install mesa 10.4.3
