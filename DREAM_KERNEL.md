# DM820 / DM7080: shared kernel bank A

These receivers store their kernel in a Dream bootblob in the raw eMMC user
area. Bank A starts at 8 MiB and is limited to 32 MiB. Bank B starts at 40 MiB;
rootfs partition p1 starts at 72 MiB. The hardware devices `mmcblk0boot0/1`
are unrelated to these banks.

## Input and preparation

The image directory contains one **raw** `kernel.bin` and `rootfs.tar.bz2`
(or `.tar.xz`). Copy `/boot/vmlinux.bin` with symlink dereferencing when
building the ZIP. Backups must instead export physical A as described below.
Do not supply a dummy, ELF executable,
gzip kernel, or an already wrapped bootblob as `kernel.bin`.

The sole kernel input is the external `kernel.bin`. Its size, raw format and
model-specific Linux release banner are checked before any flash modification.
It is not compared with `/boot` in the rootfs. Kernel preparation never opens
or decompresses the rootfs archive; the archive is unpacked once, during the
actual rootfs installation. The same preparation is used for `-k` and `-k -r`.

The LCD animation is compiled into `dream_lcd.c` and selected by model. The
original `/usr/share/fastboot/lcd_anim.bin` payloads are different for DM820
(928 bytes, fastboot 1.0) and DM7080 (1248 bytes, fastboot 1.3). Both source
packages were verified against the SHA256 values in the OE recipes. Payload
provenance is recorded in that source file. No installed animation file is
required. Build and backup images still contain their kernel and animation
under the standard rootfs paths for the existing Dream recovery interface.

The integrated formatter creates a zero-filled 512-byte little-endian entry
table followed by 4 KiB padded animation and kernel payloads. Payload positions
are relative sectors; the kernel load address is `0x1000`, and the animation
is allocated below `0x10000000`. No `mkbootblob`, `flash-kernel`, `dd`, or other
external kernel preparation/writing program is invoked.

## Target and write behaviour

Only DM820 and DM7080 select this backend. It requires the expected eMMC layout,
512-byte sectors, no partition overlapping the raw banks, and selector
`optA\0` at byte 32768. B/C/unknown selectors fail without changing the selector.
The selector describes the configured boot source, not proof of which bank
the bootloader actually loaded.

The mounted root and Chkroot subdirectory are determined from mountinfo,
because `/proc/cmdline` may describe the root before `switch_root`.
`-mN` selects a rootfs directory; it never selects a kernel bank. All Chkroot
slots continue sharing A. `-kmmcblk0p1` and hardware boot-device overrides
are rejected. DM900/DM920 retain their existing flash path.

Before a combined write, a target mounted read-only is rejected. The blob is
prepared in RAM before the rootfs update and written after rootfs extraction.
The writer uses bounded, aligned direct I/O, checks short I/O and errors,
calls `fsync`, and compares every byte through direct readback. The selector,
bank B, partition table and hardware boot areas are not written. An interrupted
bank A update is not atomic and can require recovery.

## Integration and checks

`ofgwrite_bin --features` reports `dream-kernel-a` without probing devices.
E2 can check this capability before enabling `-k`. In the companion E2 change,
the main internal image updates A; other Chkroot images remain rootfs-only.
Older ofgwrite binaries retain the rootfs-only E2 path.

## Backing up the shared kernel

```sh
ofgwrite_bin --backup-dream-kernel-a /path/to/new-directory
```

This separate read-only command opens eMMC with `O_RDONLY | O_DIRECT`. It
requires a standard animation-plus-kernel A container and reads it twice,
rejecting changing data. Model, layout, selector, `uname` release and the full
`/proc/version` build banner must agree. Unknown/additional entries are rejected
instead of producing a backup that silently loses data. No files from the
running or selected image's `/boot` are used, and no rootfs is mounted.

The new directory contains raw `kernel.bin`, a `rootfs` overlay with the same
kernel under `boot/vmlinux.bin-<release>`, its stable relative `boot/vmlinux.bin`
link, `usr/share/fastboot/lcd_anim.bin` from A, and an executable
`kernel-image.postinst` template at the top level. The command does not replace
an existing directory. On export failure discard partial output. The container
records only padded lengths: exported payloads keep that padding. Rewrapping
these raw files reproduces the original container byte for byte; they are not
a wrapped bootblob disguised as `kernel.bin`.

E2 exports A before archiving. It excludes the selected rootfs's old raw/gzip
kernels, animation and kernel postinst, then appends the overlay files to the
uncompressed tar. It installs the postinst in the existing dpkg or opkg info
path (dpkg first, matching Dream recovery). The source rootfs stays unchanged.
An older ofgwrite without this export command causes the backup to fail; there
is no fallback to a possibly stale `/boot` kernel.

The postinst calls the existing `flash-kernel /boot/vmlinux.bin` interface, so
old recovery can consume ordinary raw kernel and animation files. ofgwrite
itself uses its integrated formatter and never invokes this postinst or any
external kernel helper. The feature token is `dream-kernel-a-backup`.

Selector and build-banner checks do not prove that every stored kernel byte
equals the already running kernel in RAM, for example after a same-banner
kernel was changed on disk since boot. This command exports physical A under
the shared-A boot policy; it cannot recover a previously overwritten kernel
from RAM.

## Validation

Use the binary directly for the read-only preflight:

```sh
ofgwrite_bin -n -k -r /path/to/image
```

The Dream `-n` path returns before framebuffer setup, mounts, process stops,
rootfs deletion, writes or reboot. The shell wrapper may set up `/newroot`,
so use `ofgwrite_bin` for this check. This preflight validates the kernel and
target layout; it does not validate the compressed rootfs contents.

Build the normal static executable with `make`.

Bank B selection is intentionally deferred. Do not introduce a per-slot bank
mapping or advertise `mmcblk0p1` as a real kernel partition on these models.
