#!/usr/bin/env python3
"""Read RPCS3 RRC v6 captures without running the game or emulator.

Format reference: RPCS3 58317c9350983d71f18df2b0a346433161d6b13c,
RSXThread.cpp serialize<frame_capture_data/rsx_state> and serialization.hpp.
Keeps original memory update ordering, display/tile states and initial registers.
Game data remains local under logs/. This is not a game save-state.
"""
import argparse
from collections import Counter
import gzip
import hashlib
import json
from pathlib import Path
import struct


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, size):
        if size < 0 or size > len(self.data) - self.pos:
            raise ValueError(f'Truncated capture at {self.pos:#x}, requested {size}')
        result = self.data[self.pos:self.pos + size]
        self.pos += size
        return result

    def unpack(self, fmt):
        return struct.unpack('<' + fmt, self.take(struct.calcsize('<' + fmt)))

    def count(self):
        result = 0
        for shift in range(0, 28, 7):
            byte, = self.unpack('B')
            result |= (byte & 127) << shift
            if not byte & 128:
                return result
        raise ValueError('Invalid container length')


def read_capture(path):
    path = Path(path)
    data = gzip.open(path, 'rb').read() if path.suffix == '.gz' else path.read_bytes()
    r = Reader(data)
    if r.unpack('III') != (0x435252, 6, 1):
        raise ValueError('Only little endian RPCS3 RRC version 6 is supported')
    def fixed_map(size):
        result = {}
        for _ in range(r.count()):
            value = r.take(size)
            key, = r.unpack('Q')
            if key in result:
                raise ValueError('Duplicate state ID')
            result[key] = value
        return result
    tiles = fixed_map(432)
    memory = {k: struct.unpack('<IIQ', v) for k, v in fixed_map(16).items()}
    blobs = {}
    for _ in range(r.count()):
        blob = r.take(r.count())
        key, = r.unpack('Q')
        if key in blobs:
            raise ValueError('Duplicate data ID')
        blobs[key] = blob
    displays = fixed_map(132)
    commands = []
    for _ in range(r.count()):
        word, value = r.unpack('II')
        ids = r.unpack('Q' * r.count())
        tile, display = r.unpack('QQ')
        if any(i not in memory for i in ids) or (tile and tile not in tiles) or (display and display not in displays):
            raise ValueError('Missing referenced state')
        commands.append((word, value, ids, tile, display))
    vp = r.take(544 * 4 * 4)
    # RRC v6 deliberately omits transform constants (unlike savestates).
    regs = r.take(0x10000)
    if r.pos != len(data):
        raise ValueError(f'Unexpected trailing data: {len(data)-r.pos} bytes')
    for off, loc, blob_id in memory.values():
        if blob_id not in blobs or loc not in (0, 1) or off + len(blobs[blob_id]) > 0x10000000:
            raise ValueError(f'Unsupported memory region: {off:#x}, location {loc}')
    return dict(tiles=tiles, memory=memory, blobs=blobs, displays=displays,
                commands=commands, vp=vp, regs=regs, uncompressed_bytes=len(data))


def methods(capture):
    remaining = method = 0
    nonincrement = False
    for index, (word, value, ids, tile, display) in enumerate(capture['commands']):
        if remaining == 0:
            remaining = (word >> 18) & 0x7ff
            method = word & 0xfffc
            nonincrement = bool(word & 0x40000000)
            if remaining == 0:  # Capture's initial NOP carrying display/tile state.
                yield index, None, value, ids, tile, display
                continue
        yield index, method, value, ids, tile, display
        remaining -= 1
        if not nonincrement:
            method += 4
    if remaining:
        raise ValueError('Incomplete command packet')


def export(capture, output):
    """Self-contained replay file, LE fixed lengths, shared blobs stored once."""
    with Path(output).open('wb') as f:
        def pack(fmt, *v):
            f.write(struct.pack('<' + fmt, *v))
        f.write(b'MSRX0001')
        f.write(capture['vp'])
        f.write(capture['regs'])
        for name, size in [('tiles',432), ('displays',132), ('blobs',None)]:
            pack('I', len(capture[name]))
            for key, data in capture[name].items():
                pack('QI', key, len(data))
                f.write(data)
        pack('I', len(capture['memory']))
        for key, (off, loc, ds) in capture['memory'].items():
            pack('QIIQ', key, off, loc, ds)
        pack('I', len(capture['commands']))
        for _, method, value, ids, tile, display in methods(capture):
            pack('IIIQQ', 0xffffffff if method is None else method, value, len(ids), tile, display)
            for key in ids:
                pack('Q',key)


def complete_display_flip(capture):
    """Complete a capture ending at DRIVER_QUEUE, without changing any draw.

    RPCS3 captures stop at on_frame_end() in sys_rsx package 0x103 (queue),
    before the next 0x102 display flip. A preceding-frame flip inside the
    capture can prevent rsx_replay.cpp's fallback flip. Append the corresponding
    queued-buffer flip to a COPY to test this presentation dependency. This
    alone did not repair the MotorStorm race-start recording's black replay;
    it does not validate the recording or supply missing rendering state.
    Reference: pinned RPCS3 sys_rsx.cpp, cases 0x102 and 0x103.
    """
    commands = list(methods(capture))
    _, method, buffer, *_ = commands[-1]
    if method not in (0xe940, 0xe944) or buffer > 7:
        raise ValueError('Capture does not end with a supported display queue')
    display = next((c[5] for c in reversed(commands) if c[5]), None)
    if display is None or buffer >= struct.unpack('<33I', capture['displays'][display])[32]:
        raise ValueError('Queued display buffer is not registered')
    result = dict(capture)
    flip_method = 0xe920 + (method - 0xe940)
    result['commands'] = capture['commands'] + [
        ((1 << 18) | flip_method, 0x80000100 | buffer, (), 0, 0)]
    return result


def write_capture(capture, output):
    """Write an RRC v6 copy, preserving every recorded state and command."""
    output = Path(output)
    with (gzip.open(output, 'wb', compresslevel=1) if output.suffix == '.gz'
          else output.open('wb')) as f:
        def pack(fmt, *v):
            f.write(struct.pack('<' + fmt, *v))
        def count(n):
            while n >= 128:
                f.write(bytes([(n & 127) | 128])); n >>= 7
            f.write(bytes([n]))
        def fixed_map(entries):
            count(len(entries))
            for key, data in entries.items():
                f.write(data); pack('Q', key)
        pack('III', 0x435252, 6, 1)
        fixed_map(capture['tiles'])
        fixed_map({k:struct.pack('<IIQ', *v) for k,v in capture['memory'].items()})
        count(len(capture['blobs']))
        for key,data in capture['blobs'].items():
            count(len(data)); f.write(data); pack('Q',key)
        fixed_map(capture['displays'])
        count(len(capture['commands']))
        for word,value,ids,tile,display in capture['commands']:
            pack('II',word,value); count(len(ids))
            for key in ids: pack('Q',key)
            pack('QQ',tile,display)
        f.write(capture['vp']); f.write(capture['regs'])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--export', type=Path)
    p.add_argument('--report', type=Path)
    p.add_argument('--complete-flip', type=Path,
                   help='Write an RRC copy with its final queued display buffer flipped')
    a = p.parse_args()
    cap = read_capture(a.capture)
    hist = Counter(m for _,m,*_ in methods(cap) if m is not None)
    report = dict(source=str(a.capture), sha256=hashlib.sha256(a.capture.read_bytes()).hexdigest(),
                  uncompressed_bytes=cap['uncompressed_bytes'], commands=len(cap['commands']),
                  memory_blocks=len(cap['memory']), data_blocks=len(cap['blobs']),
                  data_bytes=sum(map(len,cap['blobs'].values())),
                  draws=sum(m == 0x1808 and v == 0 for _,m,v,*_ in methods(cap)),
                  tiles=len(cap['tiles']), displays={str(k):list(struct.unpack('<33I',v)) for k,v in cap['displays'].items()},
                  method_counts={f'{m:04x}':n for m,n in sorted(hist.items())})
    if a.export:
        export(cap,a.export)
    if a.complete_flip:
        if a.complete_flip.resolve() == a.capture.resolve():
            raise ValueError('Keep the original capture; choose a different output path')
        write_capture(complete_display_flip(cap), a.complete_flip)
    if a.report:
        a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k != 'method_counts'},indent=2))


if __name__ == '__main__':
    main()
