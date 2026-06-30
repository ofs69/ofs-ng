#!/usr/bin/env python3
"""
Generate data/simulator.glb — the 3D model shown in the funscript simulator.

The model is authored procedurally here rather than in a DCC tool so it lives in source
control as reviewable code: a diff to the shape is a diff to this file, and the committed
.glb is just its build output. It is packed into data.pak by the normal asset glob
(cmake/PackAssets.cmake), so regenerating this file and rebuilding is all that's needed.

Coordinate frame matches the host's axis mapping (src/UI/ScriptSimulator.cpp, render3D):
Y is up (stroke), the cylinder's long axis is Y (twist rotates about it), pitch is about X,
roll about Z. Overall extents (height 1.75, radius 0.5) are kept identical to the original
Godot reference (OFS_Simulator3D/Main.tscn) so the existing simulator cameras still frame it.

Improvements over the original cylinder + two red cubes:
  * Correct, outward-facing CCW winding on every triangle. The original .glb had inconsistent
    winding, which only renders if back-face culling is off; the host now culls back faces, so
    the winding must be right. A post-pass (`Mesh.orient`) reorders each triangle to agree with
    its smooth vertex normals, so the winding is correct by construction and can't silently
    regress when the geometry is edited.
  * Smooth per-vertex normals on the curved surfaces (radial on the cylinder, spherical on the
    dome), so the host's diffuse/rim shading reads curvature instead of facets.
  * A rounded dome top instead of a flat cap, so "up" (and thus pitch / roll / stroke direction)
    is unambiguous at a glance.
  * The two twist markers are given distinct warm / cool colors. With a single color they were
    symmetric under a 180-degree twist, so twist *direction* was unreadable; differing colors
    break that symmetry.
  * Baked grey-scale ambient occlusion (a downward gradient) in a COLOR_0 vertex channel, which the
    host shader multiplies in. Zero runtime cost; it grounds the form so the bottom doesn't read as
    flatly lit as the top.

Dependency-free (Python standard library only).

Usage:
    python tools/gen_simulator_model.py
    python tools/gen_simulator_model.py --out data/simulator.glb --segments 64
"""
from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent

# --- model dimensions (model units; Y up) -----------------------------------------------------
RADIUS = 0.5
Y_BOTTOM = -0.875
Y_SHOULDER = 0.375  # where the cylinder body meets the dome; dome top = Y_SHOULDER + RADIUS = 0.875
NUB_SIZE = 0.22
NUB_X = 0.55  # nub centre; sits just proud of the body surface (RADIUS = 0.5)

# Base colors (linear-ish sRGB factors). The host shader multiplies these by lighting, so they
# stay fairly bright. The body and dome share one mesh and one color so there is no material seam at
# the shoulder; the rounded top and the host's top-lit ambient already mark "up". Nubs warm/cool.
COL_BODY = (0.80, 0.82, 0.86, 1.0)
COL_NUB_WARM = (0.86, 0.27, 0.21, 1.0)
COL_NUB_COOL = (0.20, 0.56, 0.76, 1.0)


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _norm(v):
    m = math.sqrt(_dot(v, v)) or 1.0
    return (v[0] / m, v[1] / m, v[2] / m)


class Mesh:
    """Accumulates positions, smooth normals and triangle indices for one primitive."""

    def __init__(self) -> None:
        self.pos: list[tuple[float, float, float]] = []
        self.nrm: list[tuple[float, float, float]] = []
        self.col: list[tuple[float, float, float]] = []  # COLOR_0; grey-scale baked AO, white = unoccluded
        self.idx: list[int] = []

    def add(self, p, n) -> int:
        self.pos.append((float(p[0]), float(p[1]), float(p[2])))
        self.nrm.append(_norm(n))
        self.col.append((1.0, 1.0, 1.0))
        return len(self.pos) - 1

    def tri(self, a: int, b: int, c: int) -> None:
        self.idx += [a, b, c]

    def orient(self) -> None:
        # Make every triangle wind CCW as seen from outside: the geometric face normal must agree
        # with the (outward-pointing) smooth vertex normals; if it doesn't, swap two indices.
        for t in range(0, len(self.idx), 3):
            i, j, k = self.idx[t], self.idx[t + 1], self.idx[t + 2]
            fn = _cross(_sub(self.pos[j], self.pos[i]), _sub(self.pos[k], self.pos[i]))
            ref = (
                self.nrm[i][0] + self.nrm[j][0] + self.nrm[k][0],
                self.nrm[i][1] + self.nrm[j][1] + self.nrm[k][1],
                self.nrm[i][2] + self.nrm[j][2] + self.nrm[k][2],
            )
            if _dot(fn, ref) < 0.0:
                self.idx[t + 1], self.idx[t + 2] = k, j


def cylinder_side(mesh: Mesh, radius: float, y0: float, y1: float, seg: int) -> None:
    base = len(mesh.pos)
    for s in range(seg + 1):  # duplicate seam column keeps indexing simple; normals are radial
        th = 2.0 * math.pi * s / seg
        c, si = math.cos(th), math.sin(th)
        n = (c, 0.0, si)
        mesh.add((radius * c, y0, radius * si), n)
        mesh.add((radius * c, y1, radius * si), n)
    for s in range(seg):
        b0, t0 = base + 2 * s, base + 2 * s + 1
        b1, t1 = base + 2 * (s + 1), base + 2 * (s + 1) + 1
        mesh.tri(b0, t0, t1)
        mesh.tri(b0, t1, b1)


def disk(mesh: Mesh, radius: float, y: float, ny: float, seg: int) -> None:
    n = (0.0, ny, 0.0)
    center = mesh.add((0.0, y, 0.0), n)
    rim = [mesh.add((radius * math.cos(2.0 * math.pi * s / seg), y, radius * math.sin(2.0 * math.pi * s / seg)), n)
           for s in range(seg)]
    for s in range(seg):
        mesh.tri(center, rim[s], rim[(s + 1) % seg])


def dome(mesh: Mesh, radius: float, y_center: float, seg: int, rings: int) -> None:
    # Upper hemisphere; smooth normals point radially out from the dome centre. The apex is a single
    # shared vertex fanned to the top ring, so there are no degenerate (zero-area) pole triangles.
    def vert(lon: int, lat: int):
        phi = 0.5 * math.pi * lat / rings  # 0 at equator; the top ring (lat = rings-1) stops short of the pole
        th = 2.0 * math.pi * lon / seg
        cp, sp = math.cos(phi), math.sin(phi)
        p = (radius * cp * math.cos(th), y_center + radius * sp, radius * cp * math.sin(th))
        return mesh.add(p, (cp * math.cos(th), sp, cp * math.sin(th)))

    grid = [[vert(lon, lat) for lon in range(seg + 1)] for lat in range(rings)]
    for lat in range(rings - 1):
        for lon in range(seg):
            a, b = grid[lat][lon], grid[lat][lon + 1]
            c, d = grid[lat + 1][lon], grid[lat + 1][lon + 1]
            mesh.tri(a, b, d)
            mesh.tri(a, d, c)
    apex = mesh.add((0.0, y_center + radius, 0.0), (0.0, 1.0, 0.0))
    top = grid[rings - 1]
    for lon in range(seg):
        mesh.tri(top[lon], top[lon + 1], apex)


def box(mesh: Mesh, size: float) -> None:
    h = size / 2.0
    faces = [
        ((1, 0, 0), [(h, -h, -h), (h, h, -h), (h, h, h), (h, -h, h)]),
        ((-1, 0, 0), [(-h, -h, h), (-h, h, h), (-h, h, -h), (-h, -h, -h)]),
        ((0, 1, 0), [(-h, h, -h), (-h, h, h), (h, h, h), (h, h, -h)]),
        ((0, -1, 0), [(-h, -h, h), (-h, -h, -h), (h, -h, -h), (h, -h, h)]),
        ((0, 0, 1), [(-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)]),
        ((0, 0, -1), [(h, -h, -h), (-h, -h, -h), (-h, h, -h), (h, h, -h)]),
    ]
    for n, quad in faces:
        v = [mesh.add(p, n) for p in quad]
        mesh.tri(v[0], v[1], v[2])
        mesh.tri(v[0], v[2], v[3])


def _smoothstep(a: float, b: float, x: float) -> float:
    t = 0.0 if a == b else _clamp01((x - a) / (b - a))
    return t * t * (3.0 - 2.0 * t)


def _clamp01(x: float) -> float:
    return 0.0 if x < 0.0 else 1.0 if x > 1.0 else x


def bake_ao(mesh: Mesh) -> None:
    # Cheap baked ambient occlusion as a grey-scale vertex color: a downward gradient so lower geometry
    # reads as sitting in more occlusion, grounding the form. (A contact term around the nub bases would
    # need mid-height body rings the cylinder doesn't carry — vertex AO can only darken existing
    # vertices — and adding them would undo the triangle budget, so it is deliberately left out.)
    for i, p in enumerate(mesh.pos):
        ao = 0.62 + 0.38 * _smoothstep(Y_BOTTOM, Y_BOTTOM + 1.1, p[1])
        mesh.col[i] = (ao, ao, ao)


def build_meshes(seg: int):
    # Body, dome and bottom cap are one mesh so they share a material and shade as a single continuous
    # surface (the cylinder's top ring and the dome's equator coincide with identical radial normals).
    body = Mesh()
    cylinder_side(body, RADIUS, Y_BOTTOM, Y_SHOULDER, seg)
    disk(body, RADIUS, Y_BOTTOM, -1.0, seg)
    dome(body, RADIUS, Y_SHOULDER, seg, max(6, seg // 4))

    nub = Mesh()
    box(nub, NUB_SIZE)

    for m in (body, nub):
        m.orient()
        bake_ao(m)
    return body, nub


# --- glTF / GLB serialization -----------------------------------------------------------------

def _pad(buf: bytearray, fill: int) -> None:
    while len(buf) % 4 != 0:
        buf.append(fill)


def write_glb(out_path: Path, seg: int) -> None:
    body, nub = build_meshes(seg)

    bin_blob = bytearray()
    buffer_views: list[dict] = []
    accessors: list[dict] = []

    def add_accessor(data: bytes, *, comp_type: int, acc_type: str, count: int,
                     target: int, minmax=None) -> int:
        offset = len(bin_blob)
        bin_blob.extend(data)
        _pad(bin_blob, 0)
        buffer_views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data), "target": target})
        acc = {"bufferView": len(buffer_views) - 1, "componentType": comp_type,
               "count": count, "type": acc_type}
        if minmax is not None:
            acc["min"], acc["max"] = minmax
        accessors.append(acc)
        return len(accessors) - 1

    def add_mesh_prim(mesh: Mesh, material: int) -> dict:
        pos_flat = [c for p in mesh.pos for c in p]
        nrm_flat = [c for n in mesh.nrm for c in n]
        mins = [min(p[i] for p in mesh.pos) for i in range(3)]
        maxs = [max(p[i] for p in mesh.pos) for i in range(3)]
        pos_acc = add_accessor(struct.pack(f"<{len(pos_flat)}f", *pos_flat),
                               comp_type=5126, acc_type="VEC3", count=len(mesh.pos),
                               target=34962, minmax=(mins, maxs))
        nrm_acc = add_accessor(struct.pack(f"<{len(nrm_flat)}f", *nrm_flat),
                               comp_type=5126, acc_type="VEC3", count=len(mesh.nrm), target=34962)
        col_flat = [c for col in mesh.col for c in col]
        col_acc = add_accessor(struct.pack(f"<{len(col_flat)}f", *col_flat),
                               comp_type=5126, acc_type="VEC3", count=len(mesh.col), target=34962)
        idx_acc = add_accessor(struct.pack(f"<{len(mesh.idx)}I", *mesh.idx),
                               comp_type=5125, acc_type="SCALAR", count=len(mesh.idx), target=34963)
        return {"attributes": {"POSITION": pos_acc, "NORMAL": nrm_acc, "COLOR_0": col_acc},
                "indices": idx_acc, "material": material}

    def material(color) -> dict:
        return {"pbrMetallicRoughness": {"baseColorFactor": list(color),
                                         "metallicFactor": 0.0, "roughnessFactor": 0.55}}

    materials = [material(COL_BODY), material(COL_NUB_WARM), material(COL_NUB_COOL)]

    # Two nub meshes share the nub accessors but carry different materials so the markers differ
    # in color (and thus twist direction is readable).
    nub_warm_prim = add_mesh_prim(nub, 1)
    nub_cool_prim = {**nub_warm_prim, "material": 2}
    meshes = [
        {"name": "Body", "primitives": [add_mesh_prim(body, 0)]},
        {"name": "TwistMarkerWarm", "primitives": [nub_warm_prim]},
        {"name": "TwistMarkerCool", "primitives": [nub_cool_prim]},
    ]

    nodes = [
        {"name": "Stroker", "mesh": 0, "children": [1, 2]},  # the moving root the host drives
        {"name": "TwistMarkerWarm", "mesh": 1, "translation": [NUB_X, 0.0, 0.0]},
        {"name": "TwistMarkerCool", "mesh": 2, "translation": [-NUB_X, 0.0, 0.0]},
    ]

    gltf = {
        "asset": {"version": "2.0", "generator": "ofs-ng tools/gen_simulator_model.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(bin_blob)}],
    }

    json_blob = bytearray(json.dumps(gltf, separators=(",", ":")).encode("utf-8"))
    _pad(json_blob, 0x20)  # JSON chunk pads with spaces
    _pad(bin_blob, 0)

    total = 12 + 8 + len(json_blob) + 8 + len(bin_blob)
    glb = bytearray()
    glb += struct.pack("<III", 0x46546C67, 2, total)  # 'glTF', version 2, total length
    glb += struct.pack("<II", len(json_blob), 0x4E4F534A)  # 'JSON'
    glb += json_blob
    glb += struct.pack("<II", len(bin_blob), 0x004E4942)  # 'BIN\0'
    glb += bin_blob

    out_path.write_bytes(glb)
    tris = (len(body.idx) + 2 * len(nub.idx)) // 3
    print(f"Wrote {out_path} ({len(glb)} bytes, {tris} triangles, {len(buffer_views)} accessors)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=PROJECT_ROOT / "data" / "simulator.glb",
                        help="output .glb path (default: data/simulator.glb)")
    parser.add_argument("--segments", type=int, default=32,
                        help="radial segments for the cylinder/dome (default: 32; the silhouette deviates "
                             "sub-pixel at the simulator's render sizes, so higher is wasted geometry)")
    args = parser.parse_args()
    if args.segments < 8:
        parser.error("--segments must be at least 8")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    write_glb(args.out, args.segments)


if __name__ == "__main__":
    main()
