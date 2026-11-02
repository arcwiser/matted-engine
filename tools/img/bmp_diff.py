#!/usr/bin/env python3
"""Compare uncompressed 24/32-bit BMP render dumps using only the stdlib."""

from __future__ import annotations

import argparse
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes  # RGB, top-to-bottom


@dataclass(frozen=True)
class Metrics:
    differing_pixels: int
    total_pixels: int
    mean_abs: float
    max_abs: float

    @property
    def differing_percent(self) -> float:
        return 100.0 * self.differing_pixels / self.total_pixels


def read_bmp(path: Path) -> Image:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP file")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40 or len(data) < 14 + dib_size:
        raise ValueError(f"{path}: unsupported DIB header")
    width, signed_height, planes, bpp, compression = struct.unpack_from("<iiHHI", data, 18)
    if width <= 0 or signed_height == 0 or planes != 1:
        raise ValueError(f"{path}: invalid BMP dimensions/planes")
    if bpp not in (24, 32) or compression != 0:
        raise ValueError(f"{path}: only uncompressed 24/32-bit BMP is supported")
    height = abs(signed_height)
    bytes_per_pixel = bpp // 8
    stride = (width * bytes_per_pixel + 3) & ~3
    if pixel_offset + stride * height > len(data):
        raise ValueError(f"{path}: truncated pixel data")
    rows = range(height) if signed_height < 0 else range(height - 1, -1, -1)
    rgb = bytearray(width * height * 3)
    out = 0
    for source_row in rows:
        base = pixel_offset + source_row * stride
        for x in range(width):
            b, g, r = data[base + x * bytes_per_pixel : base + x * bytes_per_pixel + 3]
            rgb[out : out + 3] = bytes((r, g, b))
            out += 3
    return Image(width, height, bytes(rgb))


def write_bmp(path: Path, image: Image) -> None:
    stride = (image.width * 3 + 3) & ~3
    pixel_bytes = stride * image.height
    header = struct.pack("<2sIHHI", b"BM", 54 + pixel_bytes, 0, 0, 54)
    dib = struct.pack("<IiiHHIIIIII", 40, image.width, image.height, 1, 24, 0,
                      pixel_bytes, 2835, 2835, 0, 0)
    body = bytearray()
    padding = bytes(stride - image.width * 3)
    for y in range(image.height - 1, -1, -1):
        for x in range(image.width):
            i = (y * image.width + x) * 3
            r, g, b = image.pixels[i : i + 3]
            body.extend((b, g, r))
        body.extend(padding)
    path.write_bytes(header + dib + body)


def compare(a: Image, b: Image, threshold: float) -> tuple[Metrics, Image]:
    if (a.width, a.height) != (b.width, b.height):
        raise ValueError(f"image sizes differ: {a.width}x{a.height} vs {b.width}x{b.height}")
    diffs = [abs(x - y) for x, y in zip(a.pixels, b.pixels)]
    cutoff = threshold * 255.0
    differing = sum(
        any(diffs[i + channel] > cutoff for channel in range(3))
        for i in range(0, len(diffs), 3)
    )
    heat = bytearray(len(a.pixels))
    for i in range(0, len(diffs), 3):
        if any(diffs[i + channel] > cutoff for channel in range(3)):
            heat[i : i + 3] = b"\xff\x00\xff"
        else:
            gray = sum(a.pixels[i : i + 3]) // 3
            heat[i : i + 3] = bytes((gray, gray, gray))
    metrics = Metrics(differing, a.width * a.height,
                      sum(diffs) / (len(diffs) * 255.0), max(diffs, default=0) / 255.0)
    return metrics, Image(a.width, a.height, bytes(heat))


def selftest() -> int:
    a = Image(2, 1, bytes((0, 0, 0, 10, 20, 30)))
    b = Image(2, 1, bytes((0, 0, 0, 20, 40, 60)))
    expected_mean = 60 / (6 * 255)
    with tempfile.TemporaryDirectory() as directory:
        first, second = Path(directory) / "a.bmp", Path(directory) / "b.bmp"
        write_bmp(first, a)
        write_bmp(second, b)
        metrics, heat = compare(read_bmp(first), read_bmp(second), 0.02)
    checks = (
        metrics.differing_pixels == 1,
        metrics.total_pixels == 2,
        abs(metrics.differing_percent - 50.0) < 1e-12,
        abs(metrics.mean_abs - expected_mean) < 1e-12,
        abs(metrics.max_abs - 30 / 255) < 1e-12,
        heat.pixels[3:6] == b"\xff\x00\xff",
    )
    if not all(checks):
        print(f"SELFTEST FAIL: {metrics}", file=sys.stderr)
        return 1
    print("SELFTEST OK: differing=50.0000% mean_abs=0.039216 max_abs=0.117647")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", nargs="?", type=Path)
    parser.add_argument("second", nargs="?", type=Path)
    parser.add_argument("--heatmap", type=Path)
    parser.add_argument("--thresh", type=float, default=0.02)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.first is None or args.second is None:
        parser.error("two BMP paths are required")
    if not 0.0 <= args.thresh <= 1.0:
        parser.error("--thresh must be between 0 and 1")
    try:
        metrics, heat = compare(read_bmp(args.first), read_bmp(args.second), args.thresh)
        if args.heatmap:
            write_bmp(args.heatmap, heat)
    except (OSError, ValueError) as exc:
        print(f"bmp_diff: error: {exc}", file=sys.stderr)
        return 2
    print(f"pixels_differing={metrics.differing_percent:.4f}% "
          f"mean_abs={metrics.mean_abs:.6f} max_abs={metrics.max_abs:.6f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
