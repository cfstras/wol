#!/bin/bash
set -euxo pipefail

make clean || :
make distclean || :

./autogen.sh
./configure \
    --disable-dependency-tracking \
    --prefix=$(pwd)/sysprefix \
    --mandir=$(pwd)/sysprefix/mandir
make
