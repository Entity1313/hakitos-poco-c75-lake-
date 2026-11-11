#!/usr/bin/env python3
"""
Put a bare-metal program in front of Android's kernel in a boot image (header v2, v3 or v4).

  chainboot.py [--partition-size BYTES] <android-boot.img> <program.bin> <out.img> [payload.bin]

LK decompresses the kernel to 0x40080000 and enters it there. The new kernel is gzip of:
program.bin, padded to CHAIN_OFFSET, then Android's own (uncompressed) Image. The program runs
first and then moves the Image down to 0x40080000 and starts it (mtkloader/fbdiag.c). An optional
payload (mtkloader's "XNUP") follows the Image, 16-aligned. The program's own arm64 header gets
the Image's length in res2 (0x28), the payload's offset in res3 (0x30) and a covering image_size.
Everything else - header fields, cmdline, ramdisk, dtb - is copied from the Android image.

v2 is what the Infinix Hot 20's Android 12 ships; v3/v4 is Android 13 and later, where the header
carries no second stage, no recovery dtbo and no dtb (those fields hold a cmdline instead), the page
is 4096, and the generic ramdisk lives in init_boot rather than here. An AVB footer at the end of
the file is not carried over: it signed the old kernel, and an unlocked bootloader does not check it.
"""
import gzip
import hashlib
import os
import struct
import sys

CHAIN_OFFSET = 0x40000
# The size of boot_a. It is not the same on every phone - 32 MB on the Infinix Hot 20,
# 64 MB on the POCO C75 - and fastboot refuses an image any larger, so the check below is
# per device: CONFIG_BOOT_PARTITION in .config, which `make bootimg` passes in the
# environment, or --partition-size for a script that calls this directly. The default is
# this repository's own board.
BOOT_PARTITION = 64 << 20


def pad(b, n):
    return b + b'\0' * (-len(b) % n)


def main():
    args = sys.argv[1:]
    partition = int(os.environ.get('BOOT_PARTITION') or BOOT_PARTITION)
    if args and args[0] == '--partition-size':
        partition = int(args[1], 0)
        args = args[2:]
    if len(args) < 3:
        raise SystemExit(__doc__)
    src, prog_path, out = args[:3]
    payload = open(args[3], 'rb').read() if len(args) > 3 else b''
    d = open(src, 'rb').read()
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    hver = struct.unpack_from('<I', d, 40)[0]

    # v3 and v4 put a 1536-byte cmdline where v0-v2 had the id, second stage and dtb, so the two
    # layouts are read differently rather than one being bent to fit the other.
    if hver in (0, 1, 2):
        ksize, _, rsize, _, ssize, _, _, page, hver2 = struct.unpack_from('<9I', d, 8)
        assert hver2 == hver, f'header v{hver2} in a field that says v{hver}'
        assert hver == 2, f'header v{hver}: this phone needs v2 (Android 12) or v3/v4 (13 and later)'
        rdo_size = struct.unpack_from('<I', d, 1632)[0]
        dtb_size = struct.unpack_from('<I', d, 1648)[0]
        legacy = True
    elif hver in (3, 4):
        ksize, rsize = struct.unpack_from('<2I', d, 8)
        page = 4096                     # BOOT_IMAGE_HEADER_V3_PAGESIZE: not in the header
        ssize = rdo_size = dtb_size = 0
        legacy = False
    else:
        raise SystemExit(f'{src}: boot header v{hver} is not one this script knows (v2, v3, v4)')

    def pages(n):
        return (n + page - 1) // page * page

    off = page
    kernel = d[off:off + ksize]; off += pages(ksize)
    ramdisk = d[off:off + rsize]; off += pages(rsize)
    second = d[off:off + ssize]; off += pages(ssize)
    rdo = d[off:off + rdo_size]; off += pages(rdo_size)
    dtb = d[off:off + dtb_size]

    image = gzip.decompress(kernel) if kernel[:2] == b'\x1f\x8b' else kernel
    assert image[0x38:0x3c] == b'ARM\x64', 'Android kernel is not an arm64 Image'
    linux_image_size = struct.unpack_from('<Q', image, 0x10)[0]

    prog = bytearray(open(prog_path, 'rb').read())
    assert prog[0x38:0x3c] == b'ARM\x64', 'program has no arm64 header'
    assert len(prog) <= CHAIN_OFFSET, 'program too large'
    payload_off = CHAIN_OFFSET + len(pad(image, 16)) if payload else 0
    total = max(CHAIN_OFFSET + linux_image_size, payload_off + len(payload))
    struct.pack_into('<Q', prog, 0x10, (total + 0xfffff) & ~0xfffff)
    struct.pack_into('<Q', prog, 0x28, len(image))
    struct.pack_into('<Q', prog, 0x30, payload_off)
    blob = bytes(prog) + b'\0' * (CHAIN_OFFSET - len(prog)) + image
    if payload:
        blob = pad(blob, 16) + payload
    new_kernel = gzip.compress(blob, 9, mtime=0)

    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 8, len(new_kernel))
    if legacy:
        sha = hashlib.sha1()
        for part in (new_kernel, ramdisk, second, rdo, dtb):
            sha.update(part)
            sha.update(struct.pack('<I', len(part)))
        hdr[576:608] = pad(sha.digest(), 32)
    elif hver == 4:
        # In v3/v4 the bytes at 576 are inside the cmdline, so no id goes there; a boot signature
        # covered the kernel that was just replaced, so its field is cleared and its bytes dropped.
        struct.pack_into('<I', hdr, 1580, 0)

    img = bytes(hdr) + pad(new_kernel, page) + pad(ramdisk, page)
    if legacy:
        img += pad(second, page) + pad(rdo, page) + pad(dtb, page)
    # boot_a is 64 MB on the POCO C75 and 32 MB on the Infinix Hot 20 (see BOOT_PARTITION): an
    # image any larger is refused by fastboot, and a script that sends fastboot's output nowhere
    # would carry on as if it had been flashed
    if len(img) > partition:
        raise SystemExit(f'{out}: {len(img)} bytes, more than the {partition} of the boot partition '
                         f'({len(img) - partition} too many): the payload has to shrink')
    open(out, 'wb').write(img)
    print(f'{out}: {len(img)} bytes (header v{hver}, program {len(prog)}, Image {len(image)}, '
          f'payload {len(payload)}, kernel gz {len(new_kernel)})')


if __name__ == '__main__':
    main()
