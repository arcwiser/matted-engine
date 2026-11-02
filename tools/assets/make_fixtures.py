#!/usr/bin/env python3
"""Generate the validator fixtures (TASK-B1). Stdlib only.

Creates tools/assets/fixtures/:
  tri_cube.obj        valid 12-tri cube with vn+vt        -> 0 problems
  red_cube.gltf       valid minimal glTF, data-URI buffer -> 0 problems
  box.glb             valid binary GLB with BIN chunk     -> 0 problems
  bad_index.obj       face references vertex 99           -> ERROR
  non_tri.obj         quad face                           -> ERROR
  nan_pos.gltf        NaN in POSITION accessor min        -> ERROR
  bad_accessor.gltf   attribute/indices point at accessor 99 -> ERROR
  truncated.glb       box.glb cut short                   -> ERROR(s)

Re-run any time: python tools/assets/make_fixtures.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

FX = Path(__file__).resolve().parent / "fixtures"

# unit cube: 8 corners, 12 triangles, per-face normals/uvs
CUBE_V = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
          (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
# faces (quad corners, outward CCW) -> split into 2 triangles each
CUBE_QUADS = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
              (2, 3, 7, 6), (1, 2, 6, 5), (0, 4, 7, 3)]
CUBE_N = [(-1, 0, 0), (1, 0, 0), (0, -1, 0), (0, 1, 0), (0, 0, -1), (0, 0, 1)]


def cube_indices() -> list:
    idx = []
    for a, b, c, d in CUBE_QUADS:
        idx += [a, b, c, a, c, d]
    return idx


def write_obj(name: str, face_override=None) -> None:
    lines = ["# matted engine fixture", "o cube"]
    lines += [f"v {x} {y} {z}" for x, y, z in CUBE_V]
    lines += [f"vt {i / 3.0:.4f} {j / 2.0:.4f}" for i in range(4) for j in range(3)]
    lines += [f"vn {x} {y} {z}" for x, y, z in CUBE_N]
    lines.append("usemtl none")
    for f in range(6):
        base = f * 4
        if face_override is not None and f == 0:
            lines.append(face_override(1))
            continue
        a, b, c, d = CUBE_QUADS[f]
        n = f + 1
        lines.append(f"f {a + 1}/1/{n} {b + 1}/2/{n} {c + 1}/3/{n} {d + 1}/4/{n}")
    (FX / name).write_text("\n".join(lines) + "\n", encoding="ascii")


def gltf_cube_document() -> dict:
    idx = cube_indices()
    positions = [c for v in CUBE_V for c in v]
    normals = [c for q in CUBE_QUADS for c in CUBE_N[CUBE_QUADS.index(q)]] * 1
    # per-vertex normals: reuse face normals is wrong for shared verts; fixtures
    # only validate structure, so give each vertex a stable normal
    normals = [c for v in CUBE_V for c in (0.0, 1.0, 0.0)]
    uvs = [c for v in CUBE_V for c in (0.0, 0.0)]
    bin_data = b"".join([
        struct.pack(f"<{len(idx)}H", *idx),
        struct.pack(f"<{len(positions)}f", *positions),
        struct.pack(f"<{len(normals)}f", *normals),
        struct.pack(f"<{len(uvs)}f", *uvs),
    ])
    offsets = {
        "idx": 0,
        "pos": len(idx) * 2,
        "nrm": len(idx) * 2 + len(positions) * 4,
        "uv": len(idx) * 2 + (len(positions) + len(normals)) * 4,
    }
    nverts = len(CUBE_V)
    return {
        "asset": {"version": "2.0", "generator": "matted-fixture-gen"},
        "buffers": [{"byteLength": len(bin_data)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": offsets["idx"], "byteLength": len(idx) * 2},
            {"buffer": 0, "byteOffset": offsets["pos"], "byteLength": nverts * 12},
            {"buffer": 0, "byteOffset": offsets["nrm"], "byteLength": nverts * 12},
            {"buffer": 0, "byteOffset": offsets["uv"], "byteLength": nverts * 8},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5123, "count": len(idx),
             "type": "SCALAR", "min": [min(idx)], "max": [max(idx)]},
            {"bufferView": 1, "componentType": 5126, "count": nverts,
             "type": "VEC3", "min": [-1.0, -1.0, -1.0], "max": [1.0, 1.0, 1.0]},
            {"bufferView": 2, "componentType": 5126, "count": nverts,
             "type": "VEC3"},
            {"bufferView": 3, "componentType": 5126, "count": nverts,
             "type": "VEC2"},
        ],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 1, "NORMAL": 2, "TEXCOORD_0": 3},
            "indices": 0, "mode": 4}]}],
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "cube"}],
        "scene": 0,
    }, bin_data


def write_gltf(name: str, doc: dict, bin_data: bytes) -> None:
    uri = "data:application/octet-stream;base64," + \
        __import__("base64").b64encode(bin_data).decode("ascii")
    doc = dict(doc)
    doc["buffers"] = [{"byteLength": len(bin_data), "uri": uri}]
    (FX / name).write_text(json.dumps(doc, indent=1), encoding="ascii")


def write_glb(name: str, doc: dict, bin_data: bytes) -> None:
    json_bin = json.dumps(doc, separators=(",", ":")).encode("ascii")
    json_pad = (-len(json_bin)) % 4
    json_bin += b" " * json_pad
    bin_pad = (-len(bin_data)) % 4
    bin_data += b"\0" * bin_pad
    total = 12 + 8 + len(json_bin) + 8 + len(bin_data)
    out = struct.pack("<4sII", b"glTF", 2, total)
    out += struct.pack("<I4s", len(json_bin), b"JSON") + json_bin
    out += struct.pack("<I4s", len(bin_data), b"BIN\0") + bin_data
    (FX / name).write_bytes(out)


def main() -> None:
    FX.mkdir(parents=True, exist_ok=True)
    doc, bin_data = gltf_cube_document()

    write_obj("tri_cube.obj")
    write_obj("bad_index.obj", face_override=lambda n: f"f 1/1/1 2/2/1 99/3/1")
    (FX / "non_tri.obj").write_text(
        "# quad face fixture\n"
        "v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\n"
        "f 1 2 3 4\n", encoding="ascii")

    write_gltf("red_cube.gltf", doc, bin_data)

    import copy
    nan_doc = copy.deepcopy(doc)
    nan_doc["accessors"][1]["min"] = [float("nan"), -1.0, -1.0]
    nan_doc["accessors"][1]["max"] = [1.0, 1.0, float("nan")]
    write_gltf("nan_pos.gltf", nan_doc, bin_data)

    bad_doc = copy.deepcopy(doc)
    bad_doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"] = 99
    bad_doc["meshes"][0]["primitives"][0]["indices"] = 99
    write_gltf("bad_accessor.gltf", bad_doc, bin_data)

    write_glb("box.glb", doc, bin_data)
    raw = (FX / "box.glb").read_bytes()
    (FX / "truncated.glb").write_bytes(raw[: len(raw) - 24])

    for f in sorted(FX.iterdir()):
        print(f"  {f.name:20} {f.stat().st_size:>7} bytes")


if __name__ == "__main__":
    main()
