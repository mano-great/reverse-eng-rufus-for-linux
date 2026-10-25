rufus-linux - prebuilt static binaries
======================================

  rufus-linux-x86_64    x86-64   eb040ee9b39bb9c27b04d1b9641532fa42d101517750069fc404debffb20aa59
  rufus-linux-aarch64   AArch64  1debdba0e75b396178e917fa588c2f39513d1425efd74e0203460734bcf0dc2d

Both are fully static ELF64 binaries (musl libc). No runtime dependencies,
no Wine, no Windows code. They run on any Linux kernel >= 4.x.

Install
-------

  chmod +x rufus-linux-x86_64
  sudo install -m755 rufus-linux-x86_64 /usr/local/bin/rufus-linux

Quick start
-----------

  rufus-linux list
  rufus-linux info image.iso
  sudo rufus-linux write image.iso /dev/sdX --verify

Run 'rufus-linux --help' for all options.

Safety
------

'write' refuses to touch the disk that holds the running system, refuses
mounted targets unless --force (which unmounts them), and confirms before
writing. Writing destroys all data on the target device.

Scope
-----

This binary implements the raw-write path only: isohybrid images (Ubuntu,
Fedora, Alpine, Arch, Tiny Core, Raspberry Pi images with an MBR/GPT) are
written verbatim with optional SHA-256 verification. Formatting, ISO-image
mode and Windows To Go are not ported yet - see ../LINUX-PORT.md.

Rebuild
-------

  make            (from ../, with a normal Linux toolchain)
  zig cc -target x86_64-linux-musl -O2 -std=c11 -D_FILE_OFFSET_BITS=64 \
      -static -o rufus-linux src/rl_main.c src/rl_device.c src/rl_iso.c \
      src/rl_write.c src/rl_sha256.c src/rl_util.c
