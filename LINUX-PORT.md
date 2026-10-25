# Porting Rufus to Linux — roadmap

This fork keeps the upstream tree in `rufus-master/` and adds a native Linux
backend in `linux/`. Nothing here runs under Wine: the Linux build is compiled
against glibc/musl and talks to sysfs, `/proc` and the block-device ioctls
directly.

## Why a recompile is not enough

Measured on `rufus-master/src`:

- **36,905 lines of C**
- **522 direct Win32 API references** (excluding the vendored libraries)
- `windows.h`, `winioctl.h`, `winternl.h`, `shlobj.h` included throughout
- build system (`configure.ac`, `Makefile.am`) assumes MinGW/MSVC and embeds
  Windows `.exe` blobs for syslinux, grub4dos and ms-sys

Where the Win32 surface actually is:

| File | Win32 refs | Contains |
|---|---:|---|
| `drive.c` | 132 | VDS/COM partitioning, volume lock/unmount, drive layout |
| `dev.c` | 81 | SetupAPI enumeration, registry USB policy, port cycling |
| `stdio.c` | 35 | UTF-8 wrappers over `CreateFile`/`ReadFile` |
| `process.c` | 35 | `CreateProcess`, pipes, `ShellExecute` |
| `net.c` | 31 | WinHTTP/WinINet downloads |
| `format.c` | 27 | `FormatEx`/VDS formatting |
| `iso.c` | 27 | image parsing plus Windows file handles |
| `stdlg.c` | 21 | Win32 dialogs, `IProgressDialog`, task dialogs |

The good news: most of that is *plumbing*, not logic. The interesting parts —
filesystem layout, ISO structures, bootloader records, hashing, Windows setup
handling — are plain C data manipulation and port cleanly.

## Architecture

```
┌──────────────────────────────────────────────┐
│ UI (Win32 today, GTK4 on Linux)              │
├──────────────────────────────────────────────┤
│ Rufus logic: ISO analysis, format policy,    │
│ bootloader selection, WTG, checksums         │
├──────────────────────────────────────────────┤
│ HAL: devices, volumes, raw I/O, progress     │
├───────────────────────┬──────────────────────┤
│ Win32 backend (as-is) │ Linux backend (new)  │
└───────────────────────┴──────────────────────┘
```

The Linux backend exposes exactly the operations the logic layer needs, so
upstream `rufus.c` can be compiled against it later without rewriting the
application flow.

## Win32 → Linux mapping

| Rufus (Win32) | Linux replacement | State |
|---|---|---|
| `GetDevices()` via SetupAPI (`dev.c`) | scan `/sys/block`, read `size`, `removable`, `device/model`, realpath for USB | **done** `rl_devices_scan()` |
| `\\.\PhysicalDriveN` + `CreateFile` | `open("/dev/sdX", O_RDWR \| O_EXCL)` | **done** |
| `IOCTL_DISK_GET_LENGTH_INFO`, `IOCTL_DISK_GET_DRIVE_GEOMETRY` | `ioctl(BLKGETSIZE64)`, `ioctl(BLKSSZGET)` | **done** |
| `FSCTL_LOCK_VOLUME` / `FSCTL_UNLOCK_VOLUME` | `O_EXCL` + `flock(LOCK_EX)` | **done** |
| `FSCTL_DISMOUNT_VOLUME` | `umount2(MNT_DETACH)` | **done** |
| `SetAutoMount()` / Mount Manager | not needed; udev/kernel handles mounts | n/a |
| Registry USB "fixed disk" policy | not needed | n/a |
| `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` | libfdisk / `ioctl(BLKRRPART)` | TODO |
| VDS `IVdsAdvancedDisk::Clean/CreatePartition` | libfdisk (`fdisk_new_context`, `fdisk_add_partition`) | TODO |
| VDS `FormatPartition` / `FormatEx` | `mkfs.vfat`, `mkfs.ntfs`, `mkfs.exfat`, `mkudffs` (fork+exec) | TODO |
| `format_fat32.c` (own formatter) | `mkfs.fat` for now; port the formatter only if needed | TODO |
| `syslinux.c` + embedded `.exe`s | `grub-install`, `syslinux`, `extlinux`, `mtools` | TODO |
| `bcdboot` / `dism` / `wimgapi` (`wue.c`) | `wimlib-imagex` + prebuilt BCD hive | TODO (hardest) |
| `IProgressDialog`, `IFileDialog`, task dialogs | GTK4 `GtkProgressBar`, `GtkFileDialog`, `AdwMessageDialog` | TODO |
| `badblocks.c` (WinUSB speed/error test) | sequential `pread`/`pwrite` loop with optional `O_DIRECT` | TODO |
| `hash.c` algorithms | SHA-256 **done**; MD5/SHA-1/SHA-512 to add | partial |
| `iso.c` structure parsing (PVD, El Torito, MBR/GPT, UDF) | same offsets, now over `FILE*` | **done** `rl_iso_probe()` |

### What already has a Linux answer

- **Finding the disk** — `/sys/block` is richer than SetupAPI: model, serial,
  transport and partition topology are all plain text.
- **Not destroying the running system** — comparing `stat("/").st_dev` and
  `/proc/self/mountinfo` against the target's major:minor is more reliable than
  Rufus's drive-letter heuristics.
- **Exclusive access** — `O_EXCL` on a block device already fails if anything
  holds it, so the `FSCTL_LOCK_VOLUME` dance disappears.
- **Raw write** — `pwrite()` into `/dev/sdX` is the same operation Rufus
  performs; no driver layer needed.

### What has no clean equivalent

- **`bcdboot`** creates the Windows Boot Manager store. On Linux you must ship
  a prebuilt BCD or synthesize the registry hive (`bcdboot` is Windows-only).
  This is why full Windows To Go parity is the least certain milestone.
- **VDS** has no userspace clone; `libfdisk` covers partitioning but not
  volume formatting transactions.
- **`dism`/`wimgapi`** are replaced by `wimlib-imagex`, which is actually
  better documented, but the split/apply logic has to be rewritten.

## Milestones

| # | Milestone | Scope | Estimate |
|---|---|---|---|
| M0 | **Native write path** | enumerate, analyse, unmount, raw write, verify | **done** |
| M1 | ISO-image mode | extract non-hybrid images, install bootloader, handle `install.wim` > 4 GiB | 1–2 weeks |
| M2 | Formatting | libfdisk partitioning, FAT32/NTFS/exFAT/UDF, labels, cluster size | 2–3 weeks |
| M3 | Bootloaders | GRUB BIOS+UEFI, syslinux/extlinux, UEFI:NTFS, persistence | 2–3 weeks |
| M4 | Windows To Go | wimlib apply, BCD, unattend, BitLocker policy | 4–6 weeks |
| M5 | GTK4 UI | device list, image picker, options, progress, polkit helper | 3–4 weeks |

Full parity is realistically **4–6 months of part-time work**. M0–M2 covers
what most people actually use Rufus for (writing a Linux or Windows installer
to a stick).

## Immediate next steps

1. Port `iso.c`'s **ISO image mode**: for non-hybrid images, partition the
   target with libfdisk, create the filesystem, copy the tree, install the
   bootloader. `rl_iso_probe()` already tells you when it is required.
2. Add **GPT/MBR + FAT32 formatting** behind a `rl_format_*` API so the writer
   and formatter share the same safety checks.
3. Wrap the CLI in a **polkit-backed helper** (`org.rufuslinux.helper`) so the
   GUI never runs as root.
4. Build a **loopback test rig** (`losetup` + synthetic images) to exercise the
   destructive paths in CI.

## Testing without a Linux machine

The current workstation has no WSL/Docker/compiler, so the backend is
cross-compiled:

```sh
zig cc -target x86_64-linux-musl -O2 -std=c11 -D_FILE_OFFSET_BITS=64 -static ...
```

`linux/tests/test_logic.c` only touches portable code (ISO parsing, SHA-256), so
it compiles and runs on Windows directly. That is how the 27 assertions in the
test suite were validated before the Linux binaries were produced.

The destructive syscall paths were then validated in a **QEMU VM** running a
real Alpine kernel with a virtio disk: write, refusal-when-mounted, forced
unmount, SHA-256 read-back and partition re-read all pass, and the resulting
disk boots to an Alpine login prompt. See `linux/README.md` and
`linux/tests/vm/`.

## Licensing and naming

Rufus is **GPLv3**; this fork must stay GPLv3 and ship its source. The Rufus
name and icon belong to the upstream project, so rename the binary/UI before
distributing anything publicly (`rufus-linux` is a working title).
