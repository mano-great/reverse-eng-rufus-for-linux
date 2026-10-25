#!/bin/sh
# Build rufus-linux with the system C compiler.
set -eu
cd "$(dirname "$0")"

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -g}

echo "CC      = $CC"
echo "CFLAGS  = $CFLAGS"

$CC $CFLAGS -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -Wextra \
	-o rufus-linux \
	src/rl_main.c src/rl_device.c src/rl_iso.c src/rl_write.c \
	src/rl_sha256.c src/rl_util.c

echo "built ./rufus-linux"
