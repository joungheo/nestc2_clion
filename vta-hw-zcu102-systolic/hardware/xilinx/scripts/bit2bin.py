#!/usr/bin/env python3
"""#### JO added: convert a Vivado .bit into the .bin that /sys/class/fpga_manager loads from /lib/firmware.

The .bin is the bitstream body after the .bit header, with every 32-bit word byte-swapped (what
`bootgen -process_bitstream bin` writes). Checked 2026-10-17: zcu102_1x16_i8w8a32_16_16_19_18.bit converted
this way has the same md5 as the /lib/firmware/zcu102_1x16_i8w8a32_16_16_19_18.bin in use.

usage: bit2bin.py <in.bit> [<out.bin>]       (default out: <in>.bin next to the input)
then:  sudo cp <out.bin> /lib/firmware/        (fpga_manager only loads from there)
"""
import os, sys

def bit_body(data):
    """Raw configuration data of a .bit: skip the header fields a-d, then field 'e' = 32-bit length + data."""
    n = int.from_bytes(data[0:2], 'big')          # first field: 2-byte length + magic
    i = 2 + n
    i += 2                                        # 2-byte length (=1) before key 'a'
    while True:
        key = data[i:i + 1]; i += 1
        if key == b'e':
            size = int.from_bytes(data[i:i + 4], 'big'); i += 4
            return data[i:i + size], i
        if key not in (b'a', b'b', b'c', b'd'):
            raise SystemExit(f'unexpected .bit header field {key!r} at {i - 1}')
        flen = int.from_bytes(data[i:i + 2], 'big'); i += 2 + flen


def main():
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0] + '.bin'
    body, start = bit_body(open(src, 'rb').read())
    if len(body) % 4:
        sys.exit('bitstream length is not a multiple of 4')
    out = bytearray(len(body))
    out[0::4], out[1::4], out[2::4], out[3::4] = body[3::4], body[2::4], body[1::4], body[0::4]
    open(dst, 'wb').write(out)
    print(f'{dst}: {len(out)} bytes (header {start} bytes removed, words byte-swapped)')


if __name__ == '__main__':
    main()
