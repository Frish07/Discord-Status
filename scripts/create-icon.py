"""Rasterize the app's simple geometric brand into a multi-resolution ICO.
No dependencies; supersampling keeps the native tray/title icons crisp.
"""
from pathlib import Path
import math
import struct
import zlib


def rounded(x, y, left, top, right, bottom, radius):
    dx = max(left + radius - x, 0, x - right + radius)
    dy = max(top + radius - y, 0, y - bottom + radius)
    return left <= x <= right and top <= y <= bottom and dx * dx + dy * dy <= radius * radius


def pixel(x, y):
    color = (0, 0, 0, 0)
    if rounded(x, y, 2, 2, 46, 46, 12):
        color = (120, 101, 233, 255)
    if rounded(x, y, 13, 12, 36, 27, 5) or rounded(x, y, 13, 20, 20, 36, 2):
        color = (255, 255, 255, 255)
    if rounded(x, y, 20, 17, 29, 22, 2):
        color = (120, 101, 233, 255)
    if math.hypot(x - 42, y - 42) < 8:
        color = (12, 16, 25, 255)
    if math.hypot(x - 42, y - 42) < 5:
        color = (100, 223, 182, 255)
    return color


def png(size):
    rows = bytearray()
    samples = 4
    for y in range(size):
        rows.append(0)
        for x in range(size):
            values = [pixel((x + (sx + .5) / samples) * 48 / size,
                            (y + (sy + .5) / samples) * 48 / size)
                      for sy in range(samples) for sx in range(samples)]
            alpha = sum(v[3] for v in values)
            rows.extend(round(sum(v[c] * v[3] for v in values) / alpha) if alpha else 0 for c in range(3))
            rows.append(round(alpha / len(values)))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


sizes = [16, 20, 24, 32, 48, 64, 128, 256]
images = [png(size) for size in sizes]
offset = 6 + 16 * len(sizes)
directory = bytearray(struct.pack('<HHH', 0, 1, len(sizes)))
for size, data in zip(sizes, images):
    directory.extend(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
    offset += len(data)
(Path(__file__).resolve().parent.parent / 'assets' / 'app.ico').write_bytes(directory + b''.join(images))
