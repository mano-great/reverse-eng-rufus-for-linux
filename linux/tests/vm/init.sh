#!/usr/bin/busybox sh
# Guest-side end-to-end test for rufus-linux.
# Runs as PID 1 inside QEMU with an Alpine kernel + this payload appended to
# Alpine's netboot initramfs (which provides the virtio/vfat modules).

export PATH=/bin:/sbin:/usr/bin:/usr/sbin
FAIL=0

# Alpine's initramfs ships busybox without applet symlinks; create them.
/usr/bin/busybox --install -s /usr/bin 2>/dev/null

say() { echo ""; echo "### $*"; }
ok()  { echo "PASS: $*"; }
bad() { echo "FAIL: $*"; FAIL=1; }

need_ok() {
	if [ "$1" -eq 0 ]; then ok "$2"; else bad "$2 (rc=$1)"; fi
}

mkdir -p /proc /sys /dev /mnt/target
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

echo "=== VM TEST START ==="
uname -a
cat /proc/cmdline

# Storage + filesystem modules (Alpine's initramfs ships them under /lib/modules)
modprobe virtio_pci 2>/dev/null
modprobe virtio_blk 2>/dev/null
modprobe sd_mod 2>/dev/null
modprobe fat 2>/dev/null
modprobe vfat 2>/dev/null
sleep 1

if [ ! -b /dev/vda ]; then
	echo "FATAL: /dev/vda did not appear"
	echo "RESULT: FAIL"
	echo "=== VM TEST DONE ==="
	poweroff -f
fi
echo "disk: $(cat /sys/block/vda/size) sectors"
ls /sys/block

say "binary runs"
/rufus-linux --version
need_ok $? "rufus-linux --version"

say "list (no mounts yet)"
/rufus-linux list
need_ok $? "list exits 0"

say "analyse a real isohybrid ISO"
INFO=$(/rufus-linux info /test.iso)
RC=$?
echo "$INFO"
need_ok $RC "info exits 0"
echo "$INFO" | grep -q "Isohybrid  : yes"; need_ok $? "ISO detected as isohybrid"
echo "$INFO" | grep -q "EFI boot   : yes";   need_ok $? "EFI boot image detected"

say "create a mounted whole-disk filesystem"
dd if=/dev/zero of=/dev/vda bs=1M count=8 2>/dev/null
mkfs.vfat /dev/vda >/dev/null 2>&1
need_ok $? "mkfs.vfat /dev/vda"
mount -t vfat /dev/vda /mnt/target
need_ok $? "mount vfat on whole disk"
echo "$(df -h /mnt/target | tail -1)"

say "write must be refused while mounted"
OUT=$(/rufus-linux write /test.iso /dev/vda --yes 2>&1)
RC=$?
echo "$OUT"
echo "REFUSE_RC=$RC"
if [ $RC -eq 1 ] && echo "$OUT" | grep -q "mounted volumes"; then
	ok "mounted target refused"
else
	bad "mounted target refused"
fi
if grep -q ' /mnt/target ' /proc/mounts; then ok "target was not touched"; else bad "target was not touched"; fi
echo "MAP_BEFORE_FORCE=$(grep -c '/dev/vda ' /proc/mounts)"

say "write with --force (unmount + write + verify)"
/rufus-linux write /test.iso /dev/vda --yes --force --verify
need_ok $? "write --force --verify"
if grep -q ' /mnt/target ' /proc/mounts; then bad "volume still mounted"; else ok "volume was unmounted"; fi

say "independent checksum of the device"
IMG_SUM=$(sha256sum /test.iso | cut -d' ' -f1)
DEV_SUM=$(dd if=/dev/vda bs=1M count=67 2>/dev/null | sha256sum | cut -d' ' -f1)
echo "image=$IMG_SUM"
echo "device=$DEV_SUM"
if [ "$IMG_SUM" = "$DEV_SUM" ]; then ok "device bytes match image"; else bad "device bytes match image"; fi

say "partition table re-read"
ls -l /dev/vda* 2>&1
/rufus-linux list
need_ok $? "list after write"

say "non-isohybrid image is refused"
dd if=/dev/zero of=/plain.img bs=1M count=1 2>/dev/null
OUT=$(/rufus-linux write /plain.img /dev/vda --yes 2>&1)
RC=$?
echo "$OUT"
echo "PLAIN_RC=$RC"
if [ $RC -eq 1 ] && echo "$OUT" | grep -q "not isohybrid"; then
	ok "plain image refused"
else
	bad "plain image refused"
fi

say "--raw --dry-run overrides the guard"
OUT=$(/rufus-linux write /plain.img /dev/vda --raw --dry-run 2>&1)
RC=$?
echo "$OUT"
echo "RAW_DRY_RC=$RC"
if [ $RC -eq 0 ] && echo "$OUT" | grep -q "Dry run"; then
	ok "--raw --dry-run accepted"
else
	bad "--raw --dry-run accepted"
fi

say "missing device is rejected"
OUT=$(/rufus-linux write /test.iso /dev/doesnotexist --yes 2>&1)
RC=$?
echo "$OUT"
echo "BADDEV_RC=$RC"
if [ $RC -eq 1 ] && echo "$OUT" | grep -q "no such block device"; then
	ok "missing device rejected"
else
	bad "missing device rejected"
fi

say "verdict"
if [ $FAIL -eq 0 ]; then
	echo "RESULT: PASS"
else
	echo "RESULT: FAIL"
fi
echo "=== VM TEST DONE ==="
sync
poweroff -f
