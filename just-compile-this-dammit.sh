#!/bin/bash
set -euo pipefail
./autogen.sh
./configure \
    --disable-dependency-tracking \
    --prefix=$(pwd)/sysprefix \
    --mandir=$(pwd)/sysprefix/mandir
make
