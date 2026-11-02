#!/usr/bin/env python3
"""Matted Engine asset validator (TASK-B1).

Stdlib-only glTF / GLB / OBJ validator supporting AgentB's TASK-2 asset
pipeline. Validates single files or a whole asset pack via --manifest.

Modes:
  validate_assets.py <file.glb|.gltf|.obj> [...]     validate file(s)
  validate_assets.py --manifest assets/test/manifest.json
                                                     validate pack + expected counts
  validate_assets.py --selftest                      check fixtures + logic

Checks (per BACKLOG.md TASK-B1 spec):
  glTF: JSON parses; attribute/indices accessor references exist; decoded
        index values < POSITION vertex count; POSITION/NORMAL/TEXCOORD_0
        present on every mesh primitive; no NaN in POSITION accessor min/max;
        accessor reads stay inside their buffer.
  GLB:  magic 'glTF', version 2, length field == file size, 4-byte-aligned
        chunks, first chunk is JSON, chunk sizes sum exactly to file size.
  OBJ:  face indices within declared v/vt/vn counts; faces are triangles
        (error, with line number); warn when vn/vt are absent.

Output: one line per problem, `file:line-or-path: SEVERITY: message`.
Exit codes: 0 valid (warnings allowed) | 1 invalid (any ERROR) | 2 usage error.
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import os
import struct
import sys
from pathlib import Path

EXIT_OK = 0
EXIT_INVALID = 1
EXIT_USAGE = 2

GLB_MAGIC = b"glTF"
GLB_VERSION = 2
CHUNK_JSON = b"JSON"
CHUNK_BIN = b"BIN\0"

# glTF componentType -> (struct format char, byte size)
_COMPONENTS = {
    5120: ("b", 1),  # int8
    5121: ("B", 1),  # uint8
    5122: ("h", 2),  # int16
    5123: ("H", 2),  # uint16
    5125: ("I", 4),  # uint32
    5126: ("f", 4),  # float32
}
# glTF type -> component count
_NUM_COMPONENTS = {
    "SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
    "MAT2": 4, "MAT3": 9, "MAT4": 16,
}


class Report:
    """Collects problems in `path:location: SEVERITY: message` form."""

    def __init__(self, path: str):
        self.path = str(path)
        self.problems: list = []  # (location, severity, message)

    def error(self, location: str, message: str) -> None:
        self.problems.append((location, "ERROR", message))

    def warn(self, location: str, message: str) -> None:
        self.problems.append((location, "WARN", message))

    @property
    def has_errors(self) -> bool:
        return any(sev == "ERROR" for _, sev, _ in self.problems)

    def print(self) -> None:
        for location, sev, message in self.problems:
            print(f"{self.path}:{location}: {sev}: {message}")


# --- OBJ ---------------------------------------------------------------------


def parse_obj_vert_token(token: str, counts: dict, what: str, loc: str,
                          rep: Report) -> None:
    """Validate one `v[/vt][/vn]` (or negative) face-vertex token."""
    parts = token.split("/")
    if len(parts) > 3:
        rep.error(loc, f"malformed face vertex '{token}'")
        return
    for i, key in enumerate(("v", "vt", "vn")):
        if i >= len(parts) or parts[i] == "":
            continue
        try:
            idx = int(parts[i])
        except ValueError:
            rep.error(loc, f"non-integer {what} index '{token}'")
            return
        n = counts[key]
        if idx > 0:
            if idx > n:
                rep.error(loc, f"{what} index {idx} out of range (count {n})")
        elif idx < 0:
            if n + idx < 0:
                rep.error(loc, f"{what} relative index {idx} out of range (count {n})")


def validate_obj(path: Path, rep: Report) -> tuple:
    """Returns (tri_count, vert_count). One ERROR per bad face/line."""
    v = vt = vn = 0
    tris = 0
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        rep.error("file", f"cannot read: {exc}")
        return (0, 0)

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tokens = line.split()
        keyword, args = tokens[0], tokens[1:]
        loc = f"line {lineno}"
        if keyword == "v":
            v += 1
        elif keyword == "vt":
            vt += 1
        elif keyword == "vn":
            vn += 1
        elif keyword == "f":
            if len(args) != 3:
                rep.error(loc,
                          f"face is not a triangle ({len(args)} vertices)")
            else:
                tris += 1
            counts = {"v": v, "vt": vt, "vn": vn}
            for token in args:
                parse_obj_vert_token(token, counts, "vertex", loc, rep)
        # all other keywords (o, g, s, usemtl, mtllib, ...) are ignored

    if tris and not vn:
        rep.warn("file", "no normals (vn) present - shading will be flat/fallback")
    if tris and not vt:
        rep.warn("file", "no texcoords (vt) present - materials cannot sample UVs")
    return (tris, v)


# --- glTF core ---------------------------------------------------------------


def _bad(message: str, rep: Report, loc: str) -> bool:
    rep.error(loc, message)
    return False


def _accessor_element_size(acc: dict) -> int:
    ctype = acc.get("componentType")
    ntype = acc.get("type")
    if ctype not in _COMPONENTS or ntype not in _NUM_COMPONENTS:
        return 0
    return _COMPONENTS[ctype][1] * _NUM_COMPONENTS[ntype]


def _buffer_bytes(gltf: dict, buffer_index: int, bin_chunk: bytes | None,
                  base_dir: Path, rep: Report, loc: str) -> bytes | None:
    buffers = gltf.get("buffers") or []
    if not (0 <= buffer_index < len(buffers)):
        _bad(f"buffer index {buffer_index} out of range", rep, loc)
        return None
    buf = buffers[buffer_index]
    uri = buf.get("uri")
    if uri is None:
        if bin_chunk is None:
            _bad(f"buffer {buffer_index} has no uri and no GLB BIN chunk", rep, loc)
            return None
        return bin_chunk
    if uri.startswith("data:"):
        try:
            payload = uri.split(",", 1)[1]
            return base64.b64decode(payload)
        except Exception as exc:  # noqa: BLE001 - report any decode failure
            _bad(f"buffer {buffer_index} data URI decode failed: {exc}", rep, loc)
            return None
    ext = base_dir / uri
    try:
        return ext.read_bytes()
    except OSError as exc:
        _bad(f"buffer {buffer_index} external file '{uri}' unreadable: {exc}",
             rep, loc)
        return None


def _view_bytes(gltf: dict, bytes_by_buffer: dict, view_index: int,
                rep: Report, loc: str) -> bytes | None:
    views = gltf.get("bufferViews") or []
    if not (0 <= view_index < len(views)):
        _bad(f"bufferView index {view_index} out of range", rep, loc)
        return None
    view = views[view_index]
    bidx = view.get("buffer", -1)
    data = bytes_by_buffer.get(bidx)
    if data is None:
        data = _buffer_bytes(gltf, bidx, bytes_by_buffer.get("__bin__"),
                             bytes_by_buffer["__dir__"], rep, loc)
        if data is None:
            return None
        bytes_by_buffer[bidx] = data
    off = view.get("byteOffset", 0)
    length = view.get("byteLength", 0)
    if off < 0 or length < 0 or off + length > len(data):
        _bad(f"bufferView {view_index} range [{off}+{length}] exceeds buffer "
             f"size {len(data)}", rep, loc)
        return None
    return data[off:off + length]


def _decode_scalar_indices(gltf: dict, acc: dict, view_data: bytes) -> list | None:
    fmt, csize = _COMPONENTS[acc["componentType"]]
    off = acc.get("byteOffset", 0)
    count = acc.get("count", 0)
    end = off + count * csize
    if end > len(view_data):
        return None
    return list(struct.unpack_from(f"<{count}{fmt}", view_data, off))


def _check_accessor_bounds(gltf: dict, acc_index: int, bytes_by_buffer: dict,
                           rep: Report, loc: str) -> None:
    accessors = gltf.get("accessors") or []
    acc = accessors[acc_index]
    view_index = acc.get("bufferView")
    if view_index is None:
        return  # sparse/allowed in spec; not produced by our pipeline
    elem = _accessor_element_size(acc)
    if not elem:
        _bad(f"accessor {acc_index} has bad componentType/type", rep, loc)
        return
    view_data = _view_bytes(gltf, bytes_by_buffer, view_index, rep, loc)
    if view_data is None:
        return
    end = acc.get("byteOffset", 0) + acc.get("count", 0) * elem
    if end > len(view_data):
        _bad(f"accessor {acc_index} needs {end} bytes, bufferView "
             f"{view_index} provides {len(view_data)}", rep, loc)


def _check_mesh_primitive(gltf: dict, bytes_by_buffer: dict, mesh_i: int,
                          prim_j: int, prim: dict, rep: Report) -> None:
    loc = f"meshes[{mesh_i}].primitives[{prim_j}]"
    attrs = prim.get("attributes") or {}
    for required in ("POSITION", "NORMAL", "TEXCOORD_0"):
        if required not in attrs:
            rep.error(f"{loc}.attributes", f"missing {required}")

    pos_count = None
    for name, acc_index in attrs.items():
        aloc = f"{loc}.attributes.{name}"
        accessors = gltf.get("accessors") or []
        if not isinstance(acc_index, int) or not (0 <= acc_index < len(accessors)):
            _bad(f"accessor index {acc_index} out of range "
                 f"({len(accessors)} accessors)", rep, aloc)
            continue
        acc = accessors[acc_index]
        _check_accessor_bounds(gltf, acc_index, bytes_by_buffer, rep, aloc)
        if name == "POSITION":
            pos_count = acc.get("count")
            for bound in ("min", "max"):
                arr = acc.get(bound)
                if arr is None:
                    rep.error(f"accessors[{acc_index}]",
                              f"POSITION accessor missing '{bound}'")
                    continue
                if any(isinstance(x, float) and math.isnan(x) for x in arr):
                    rep.error(f"accessors[{acc_index}].{bound}",
                              "NaN in POSITION bounds")

    idx_acc = prim.get("indices")
    if idx_acc is not None:
        iloc = f"{loc}.indices"
        accessors = gltf.get("accessors") or []
        if not isinstance(idx_acc, int) or not (0 <= idx_acc < len(accessors)):
            _bad(f"indices accessor index {idx_acc} out of range", rep, iloc)
        else:
            acc = accessors[idx_acc]
            _check_accessor_bounds(gltf, idx_acc, bytes_by_buffer, rep, iloc)
            if acc.get("componentType") not in _COMPONENTS:
                _bad(f"indices accessor {idx_acc} bad componentType", rep, iloc)
            elif acc.get("type") != "SCALAR":
                _bad(f"indices accessor {idx_acc} type must be SCALAR, "
                     f"got {acc.get('type')}", rep, iloc)
            elif pos_count is not None:
                view_index = acc.get("bufferView")
                if view_index is not None:
                    view_data = _view_bytes(gltf, bytes_by_buffer, view_index,
                                            rep, iloc)
                    if view_data is not None:
                        values = _decode_scalar_indices(gltf, acc, view_data)
                        if values is None:
                            _bad(f"indices accessor {idx_acc} reads past its "
                                 f"bufferView", rep, iloc)
                        else:
                            bad = [v for v in values if v >= pos_count]
                            if bad:
                                _bad(f"index value {bad[0]} >= vertex count "
                                     f"{pos_count} ({len(bad)} of "
                                     f"{len(values)} indices bad)", rep, iloc)


def validate_gltf_json(path: Path, gltf: dict, bin_chunk: bytes | None,
                       rep: Report) -> tuple:
    """Shared mesh/accessor validation. Returns (tri_count, vert_count)."""
    if not isinstance(gltf, dict):
        _bad("document is not a JSON object", rep, "json")
        return (0, 0)
    asset = gltf.get("asset")
    if not isinstance(asset, dict) or not asset.get("version"):
        rep.warn("asset", "missing or versionless asset object")
    bytes_by_buffer = {"__bin__": bin_chunk, "__dir__": path.parent}
    tris = 0
    verts = 0
    meshes = gltf.get("meshes") or []
    accessors = gltf.get("accessors") or []
    for i, mesh in enumerate(meshes):
        prims = mesh.get("primitives") or []
        if not prims:
            rep.warn(f"meshes[{i}]", "mesh has no primitives")
        for j, prim in enumerate(prims):
            _check_mesh_primitive(gltf, bytes_by_buffer, i, j, prim, rep)
            mode = prim.get("mode", 4)
            pos_ref = (prim.get("attributes") or {}).get("POSITION")
            if isinstance(pos_ref, int) and 0 <= pos_ref < len(accessors):
                verts += accessors[pos_ref].get("count", 0)
            idx_ref = prim.get("indices")
            if isinstance(idx_ref, int) and 0 <= idx_ref < len(accessors):
                n = accessors[idx_ref].get("count", 0)
            elif pos_ref is not None and isinstance(pos_ref, int) \
                    and 0 <= pos_ref < len(accessors):
                n = accessors[pos_ref].get("count", 0)
            else:
                n = 0
            if mode == 4:
                tris += n // 3
            else:
                rep.warn(f"meshes[{i}].primitives[{j}]",
                         f"primitive mode {mode} is not TRIANGLES(4); "
                         f"not counted as triangles")
    return (tris, verts)


def validate_gltf(path: Path, rep: Report) -> tuple:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        rep.error("file", f"cannot read: {exc}")
        return (0, 0)
    try:
        gltf = json.loads(text)
    except json.JSONDecodeError as exc:
        rep.error("json", f"parse error: {exc}")
        return (0, 0)
    return validate_gltf_json(path, gltf, None, rep)


# --- GLB container -----------------------------------------------------------


def validate_glb(path: Path, rep: Report) -> tuple:
    try:
        data = path.read_bytes()
    except OSError as exc:
        rep.error("file", f"cannot read: {exc}")
        return (0, 0)

    if len(data) < 12:
        rep.error("container", f"file too small for GLB header ({len(data)} bytes)")
        return (0, 0)
    magic, version, total = struct.unpack_from("<4sII", data, 0)
    if magic != GLB_MAGIC:
        rep.error("container", f"bad magic {magic!r} (expected b'glTF')")
        return (0, 0)
    if version != GLB_VERSION:
        rep.error("container", f"unsupported glTF version {version} (want 2)")
    if total != len(data):
        rep.error("container", f"header length {total} != file size {len(data)}")

    gltf = None
    bin_chunk = None
    offset = 12
    first = True
    while offset + 8 <= len(data):
        clen, ctype = struct.unpack_from("<I4s", data, offset)
        if clen % 4:
            rep.error(f"chunk@{offset}", f"chunk length {clen} not 4-byte aligned")
        if offset + 8 + clen > len(data):
            rep.error(f"chunk@{offset}",
                      f"chunk overruns file ({offset + 8 + clen} > {len(data)})")
            break
        body = data[offset + 8: offset + 8 + clen]
        if first:
            if ctype != CHUNK_JSON:
                rep.error(f"chunk@{offset}", "first chunk is not JSON")
                break
            try:
                gltf = json.loads(body.decode("utf-8-sig"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                rep.error("json", f"GLB JSON chunk parse error: {exc}")
                break
            first = False
        elif ctype == CHUNK_BIN and bin_chunk is None:
            bin_chunk = body
        offset += 8 + clen

    if first:
        return (0, 0)  # never got a JSON chunk; error already reported
    if offset != len(data):
        rep.error("container", "chunk lengths inconsistent with file size")
    return validate_gltf_json(path, gltf or {}, bin_chunk, rep)


# --- manifest ----------------------------------------------------------------


def count_geometry(path: Path, rep: Report) -> tuple:
    if path.suffix.lower() == ".obj":
        return validate_obj(path, rep)
    if path.suffix.lower() == ".gltf":
        return validate_gltf(path, rep)
    if path.suffix.lower() == ".glb":
        return validate_glb(path, rep)
    rep.error("file", f"unsupported extension '{path.suffix}'")
    return (0, 0)


def validate_manifest(manifest_path: Path, rep: Report) -> bool:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        rep.error("manifest", f"cannot load: {exc}")
        return False
    if not isinstance(manifest, list):
        rep.error("manifest", "expected a JSON array of entries")
        return False
    base = manifest_path.parent
    ok = True
    for i, entry in enumerate(manifest):
        loc = f"entry[{i}]"
        if not isinstance(entry, dict) or "file" not in entry:
            rep.error(loc, "entry missing 'file'")
            ok = False
            continue
        target = base / entry["file"]
        sub = Report(target)
        tris, verts = count_geometry(target, sub)
        sub.print()
        if sub.has_errors:
            ok = False
        for key, actual in (("expected_tris", tris), ("expected_verts", verts)):
            if key in entry and entry[key] != actual:
                rep.error(f"{loc}.{key}",
                          f"manifest says {entry[key]}, file has {actual}")
                ok = False
    return ok


# --- dispatch / selftest -----------------------------------------------------


def validate_path(target: Path, rep: Report) -> tuple:
    ext = target.suffix.lower()
    if ext == ".obj":
        return validate_obj(target, rep)
    if ext == ".gltf":
        return validate_gltf(target, rep)
    if ext == ".glb":
        return validate_glb(target, rep)
    rep.error("file", f"unsupported extension '{target.suffix}' "
                      f"(want .glb/.gltf/.obj)")
    return (0, 0)


def selftest() -> int:
    """Run against the shipped fixtures; each must produce the expected result."""
    fx = Path(__file__).resolve().parent / "fixtures"
    # (filename, want_errors, want_warn, want_tris, want_verts)
    # want_errors = -1 means "at least one ERROR expected" (corrupt fixtures).
    cases = [
        ("tri_cube.obj", 0, 0, 12, 8),
        ("bad_index.obj", -1, None, None, None),
        ("non_tri.obj", -1, None, None, None),
        ("red_cube.gltf", 0, 0, 12, 8),
        ("nan_pos.gltf", -1, None, None, None),
        ("bad_accessor.gltf", -1, None, None, None),
        ("box.glb", 0, 0, 12, 8),
        ("truncated.glb", -1, None, None, None),
    ]
    failures = 0
    for name, want_err, want_warn, want_tris, want_verts in cases:
        path = fx / name
        rep = Report(path)
        tris, verts = validate_path(path, rep)
        n_err = sum(1 for _, s, _ in rep.problems if s == "ERROR")
        n_warn = sum(1 for _, s, _ in rep.problems if s == "WARN")
        problems = []
        if want_err < 0:
            if n_err < 1:
                problems.append(f"expected at least one ERROR, got {n_err}")
        elif n_err != want_err:
            problems.append(f"errors {n_err} != expected {want_err}")
        if want_tris is not None and (tris, verts) != (want_tris, want_verts):
            problems.append(f"geometry ({tris} tris, {verts} verts) != "
                            f"expected ({want_tris}, {want_verts})")
        if want_warn is not None and n_warn < want_warn:
            problems.append(f"warnings {n_warn} < expected {want_warn}")
        if problems:
            failures += 1
            print(f"selftest FAIL: {name}: {'; '.join(problems)}")
            rep.print()
        else:
            print(f"selftest PASS: {name} "
                  f"({n_err} errors, {n_warn} warnings, "
                  f"{tris} tris, {verts} verts)")

    # manifest mode: example manifest must pass with matching counts
    mrep = Report(fx / "manifest.example.json")
    manifest_ok = validate_manifest(fx / "manifest.example.json", mrep)
    if manifest_ok and not mrep.has_errors:
        print("selftest PASS: manifest.example.json (3 entries, counts match)")
    else:
        failures += 1
        print("selftest FAIL: manifest.example.json")
        mrep.print()

    print(f"selftest: {len(cases) + 1 - failures}/{len(cases) + 1} checks behaved "
          f"as expected")
    return EXIT_INVALID if failures else EXIT_OK


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Matted Engine asset validator (glTF/GLB/OBJ)")
    ap.add_argument("files", nargs="*", help=".glb/.gltf/.obj files to validate")
    ap.add_argument("--manifest", help="asset pack manifest.json to validate")
    ap.add_argument("--selftest", action="store_true",
                    help="validate shipped fixtures and check expectations")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.files and not args.manifest:
        ap.print_usage()
        print("error: give file(s) or --manifest", file=sys.stderr)
        return EXIT_USAGE

    invalid = False
    if args.manifest:
        rep = Report(args.manifest)
        if not validate_manifest(Path(args.manifest), rep):
            invalid = True
        rep.print()
    for f in args.files:
        path = Path(f)
        rep = Report(path)
        if not path.is_file():
            print(f"{path}: file: ERROR: not found")
            invalid = True
            continue
        tris, verts = validate_path(path, rep)
        rep.print()
        if rep.has_errors:
            invalid = True
        elif not rep.problems:
            print(f"{path}: OK ({tris} tris, {verts} verts)")
    return EXIT_INVALID if invalid else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
