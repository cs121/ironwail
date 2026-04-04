#!/usr/bin/env python3
"""
lightgrid_ericw_octree_to_raw_core_ericw.py

Deterministic parser for EricW-Tools BSPX LIGHTGRID_OCTREE, matching bspxfile.cc:

- Header:
    vec3f grid_dist
    vec3i grid_size
    vec3f grid_mins
    uint8 num_styles
    uint32 root_node

- Nodes:
    uint32 node_count
    node_count * (vec3i division_point + 8*uint32 children)

- Leafs:
    uint32 leaf_count
    For each leaf:
      vec3i mins
      vec3i size
      For z in [0..size.z), y in [0..size.y), x in [0..size.x):
          lightgrid_samples_t sample (variable length):
              uint8 used_styles_in
                0xFF => occluded
                else:
                  repeat used_styles_in times:
                    uint8 style
                    qvec3b color (3*uint8)

Converter:
- Uses a template Ironwail RAW v2 file to copy nx/ny/nz + cellsize + origin so the engine loads it.
- Chooses preferred style (default 0) if present, else first.
- Writes: RGB (0..1), DIR=(0,0,1) (octree doesn't store direction), INTENSITY=avg(rgb), AO=1 (or 0 if occluded).
"""

from dataclasses import dataclass
from typing import List, Tuple, Optional, Callable
import os, struct, json

# ---------------- Ironwail RAW v2 ----------------
RAW_V2_MAGIC  = 71595300
RAW_V2_VER    = 2
RAW_ENCODING_FLOAT32 = 0

RAW_COMP_RGB       = 1
RAW_COMP_DIR       = 2
RAW_COMP_INTENSITY = 4
RAW_COMP_AO        = 8

RAW_V2_HEADER_SIZE = 76

def read_raw_v2_header(path: str):
    data = open(path, "rb").read(RAW_V2_HEADER_SIZE)
    if len(data) != RAW_V2_HEADER_SIZE:
        raise ValueError("File too small to be RAW v2.")
    magic, ver, enc, flags, comps = struct.unpack_from("<IHHII", data, 0)
    if magic != RAW_V2_MAGIC or ver != RAW_V2_VER:
        raise ValueError(f"Not RAW v2 (magic/ver mismatch): {magic}/{ver}")
    nx, ny, nz = struct.unpack_from("<3I", data, 48)
    cell = struct.unpack_from("<f", data, 60)[0]
    ox, oy, oz = struct.unpack_from("<3f", data, 64)
    return {"nx": nx, "ny": ny, "nz": nz, "cellsize": cell, "origin": (ox,oy,oz), "encoding": enc, "components": comps, "flags": flags}

def write_raw_v2(path: str, dims: Tuple[int,int,int], cellsize: float, origin: Tuple[float,float,float], cells):
    nx, ny, nz = dims
    reserved = [0] * 8
    components = RAW_COMP_RGB | RAW_COMP_DIR | RAW_COMP_INTENSITY | RAW_COMP_AO
    header = struct.pack("<IHHII8IIII f 3f",
                         RAW_V2_MAGIC, RAW_V2_VER, RAW_ENCODING_FLOAT32,
                         0, components, *reserved,
                         nx, ny, nz, float(cellsize), *origin)
    if len(header) != RAW_V2_HEADER_SIZE:
        raise RuntimeError(f"RAW v2 header size mismatch: {len(header)} != {RAW_V2_HEADER_SIZE}")

    with open(path, "wb") as f:
        f.write(header)
        for rgb, dirv, inten, ao in cells:
            f.write(struct.pack("<8f",
                                float(rgb[0]), float(rgb[1]), float(rgb[2]),
                                float(dirv[0]), float(dirv[1]), float(dirv[2]),
                                float(inten), float(ao)))

# ---------------- BSPX parsing ----------------

def parse_bspx_lumps(bsp: bytes):
    idx = bsp.find(b"BSPX")
    if idx < 0:
        raise ValueError("No BSPX chunk found in BSP.")
    num = struct.unpack_from("<I", bsp, idx + 4)[0]
    base = idx + 8
    lumps = []
    for i in range(num):
        off = base + i * 32
        name = bsp[off:off+24].split(b"\x00", 1)[0].decode("ascii", "replace")
        loff, llen = struct.unpack_from("<2I", bsp, off + 24)
        lumps.append((name, loff, llen))
    return lumps

def get_bspx_lump(bsp: bytes, name: str) -> bytes:
    for n, off, ln in parse_bspx_lumps(bsp):
        if n == name:
            return bsp[off:off+ln]
    raise ValueError(f"BSPX lump '{name}' not found.")

# ---------------- EricW LIGHTGRID_OCTREE ----------------
FLAG_LEAF     = 1 << 31
FLAG_OCCLUDED = 1 << 30

@dataclass
class Header:
    grid_dist: Tuple[float,float,float]
    grid_size: Tuple[int,int,int]
    grid_mins: Tuple[float,float,float]
    num_styles: int
    root_node: int

@dataclass
class Node:
    division_point: Tuple[int,int,int]
    children: Tuple[int,int,int,int,int,int,int,int]

@dataclass
class Sample:
    style: int
    color: Tuple[int,int,int]

@dataclass
class SampleSet:
    occluded: bool
    samples: List[Sample]   # length = used_styles

@dataclass
class Leaf:
    mins: Tuple[int,int,int]
    size: Tuple[int,int,int]
    samples: List[SampleSet]  # flattened z-major like ericw loops

    def index(self, x:int, y:int, z:int)->int:
        sx,sy,sz = self.size
        return (sx*sy*z) + (sx*y) + x

class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.off = 0
    def read(self, n: int) -> bytes:
        if self.off + n > len(self.data):
            raise EOFError("Unexpected EOF")
        b = self.data[self.off:self.off+n]
        self.off += n
        return b
    def u8(self)->int: return self.read(1)[0]
    def u32(self)->int: return struct.unpack("<I", self.read(4))[0]
    def i32(self)->int: return struct.unpack("<i", self.read(4))[0]
    def f32(self)->float: return struct.unpack("<f", self.read(4))[0]
    def vec3f(self): return (self.f32(), self.f32(), self.f32())
    def vec3i(self): return (self.i32(), self.i32(), self.i32())
    def qvec3b(self): 
        b = self.read(3)
        return (b[0], b[1], b[2])

def read_lightgrid_samples(r: Reader) -> SampleSet:
    used = r.u8()
    if used == 0xFF:
        return SampleSet(occluded=True, samples=[])
    samples: List[Sample] = []
    for _ in range(used):
        st = r.u8()
        col = r.qvec3b()
        samples.append(Sample(style=st, color=col))
    return SampleSet(occluded=False, samples=samples)

def parse_octree(buf: bytes) -> Tuple[Header, List[Node], List[Leaf]]:
    r = Reader(buf)
    header = Header(
        grid_dist=r.vec3f(),
        grid_size=r.vec3i(),
        grid_mins=r.vec3f(),
        num_styles=r.u8(),
        root_node=r.u32()
    )

    node_count = r.u32()
    nodes: List[Node] = []
    for _ in range(node_count):
        div = r.vec3i()
        kids = tuple(r.u32() for _ in range(8))
        nodes.append(Node(division_point=div, children=kids))

    leaf_count = r.u32()
    leafs: List[Leaf] = []
    for _ in range(leaf_count):
        mins = r.vec3i()
        size = r.vec3i()
        sx,sy,sz = size
        total = sx*sy*sz
        samps = [read_lightgrid_samples(r) for __ in range(total)]
        leafs.append(Leaf(mins=mins, size=size, samples=samps))

    return header, nodes, leafs

def child_index(division_point: Tuple[int,int,int], test_point: Tuple[int,int,int]) -> int:
    sx = 1 if test_point[0] >= division_point[0] else 0
    sy = 1 if test_point[1] >= division_point[1] else 0
    sz = 1 if test_point[2] >= division_point[2] else 0
    return (4*sx) + (2*sy) + sz

def octree_lookup(nodes: List[Node], leafs: List[Leaf], node_index: int, test_point: Tuple[int,int,int]) -> Optional[SampleSet]:
    if node_index & FLAG_OCCLUDED:
        return SampleSet(occluded=True, samples=[])
    if node_index & FLAG_LEAF:
        li = node_index & ~FLAG_LEAF
        leaf = leafs[li]
        lx = max(0, min(test_point[0] - leaf.mins[0], leaf.size[0]-1))
        ly = max(0, min(test_point[1] - leaf.mins[1], leaf.size[1]-1))
        lz = max(0, min(test_point[2] - leaf.mins[2], leaf.size[2]-1))
        idx = leaf.index(lx,ly,lz)
        if idx < 0 or idx >= len(leaf.samples):
            return None
        return leaf.samples[idx]

    n = nodes[node_index]
    ci = child_index(n.division_point, test_point)
    return octree_lookup(nodes, leafs, n.children[ci], test_point)

def sample_at_world(header: Header, nodes: List[Node], leafs: List[Leaf], world_point: Tuple[float,float,float]) -> Optional[SampleSet]:
    lx = (world_point[0] - header.grid_mins[0]) / header.grid_dist[0]
    ly = (world_point[1] - header.grid_mins[1]) / header.grid_dist[1]
    lz = (world_point[2] - header.grid_mins[2]) / header.grid_dist[2]
    tp = (int(round(lx)), int(round(ly)), int(round(lz)))
    gs = header.grid_size
    if tp[0] < 0 or tp[1] < 0 or tp[2] < 0 or tp[0] >= gs[0] or tp[1] >= gs[1] or tp[2] >= gs[2]:
        return None
    return octree_lookup(nodes, leafs, header.root_node, tp)

def choose_style(samples: List[Sample], preferred_style: int) -> Optional[Sample]:
    if not samples:
        return None
    for s in samples:
        if s.style == preferred_style:
            return s
    return samples[0]

def rgb8_to_float(rgb: Tuple[int,int,int]) -> Tuple[float,float,float]:
    return (rgb[0]/255.0, rgb[1]/255.0, rgb[2]/255.0)

def intensity_from_rgb(rgb: Tuple[float,float,float]) -> float:
    return max(0.0, (rgb[0]+rgb[1]+rgb[2]) / 3.0)

def convert_bspx_octree_to_ironwail_raw(
    bsp_path: str,
    template_raw_path: str,
    out_raw_path: str,
    debug_json_path: Optional[str] = None,
    preferred_style: int = 0,
    progress_cb: Optional[Callable[[int,int],None]] = None
) -> dict:
    bsp = open(bsp_path, "rb").read()
    octbuf = get_bspx_lump(bsp, "LIGHTGRID_OCTREE")

    tpl = read_raw_v2_header(template_raw_path)
    out_dims = (int(tpl["nx"]), int(tpl["ny"]), int(tpl["nz"]))
    out_cell = float(tpl["cellsize"])
    out_origin = tuple(float(v) for v in tpl["origin"])

    header, nodes, leafs = parse_octree(octbuf)

    nx, ny, nz = out_dims
    ox, oy, oz = out_origin
    cs = out_cell

    default_dir = (0.0,0.0,1.0)
    cells = []
    missing = 0
    occluded = 0

    for z in range(nz):
        if progress_cb:
            progress_cb(z, nz)
        wz = oz + (z + 0.5) * cs
        for y in range(ny):
            wy = oy + (y + 0.5) * cs
            for x in range(nx):
                wx = ox + (x + 0.5) * cs
                ss = sample_at_world(header, nodes, leafs, (wx,wy,wz))
                if ss is None:
                    missing += 1
                    cells.append(((0.0,0.0,0.0), default_dir, 0.0, 0.0))
                    continue
                if ss.occluded:
                    occluded += 1
                    cells.append(((0.0,0.0,0.0), default_dir, 0.0, 0.0))
                    continue
                samp = choose_style(ss.samples, preferred_style)
                if samp is None:
                    cells.append(((0.0,0.0,0.0), default_dir, 0.0, 1.0))
                    continue
                rgb = rgb8_to_float(samp.color)
                inten = intensity_from_rgb(rgb)
                cells.append((rgb, default_dir, inten, 1.0))

    if progress_cb:
        progress_cb(nz, nz)

    write_raw_v2(out_raw_path, out_dims, out_cell, out_origin, cells)

    dbg = {
        "bsp": os.path.abspath(bsp_path),
        "template_raw": os.path.abspath(template_raw_path),
        "out_raw": os.path.abspath(out_raw_path),
        "template_header_used": {"dims": out_dims, "cellsize": out_cell, "origin": out_origin},
        "octree_header": {
            "grid_dist": header.grid_dist,
            "grid_size": header.grid_size,
            "grid_mins": header.grid_mins,
            "num_styles": header.num_styles,
            "root_node": header.root_node,
        },
        "node_count": len(nodes),
        "leaf_count": len(leafs),
        "preferred_style": preferred_style,
        "cells_written": nx*ny*nz,
        "cells_missing": missing,
        "cells_occluded": occluded,
    }
    if debug_json_path:
        with open(debug_json_path, "w", encoding="utf-8") as f:
            json.dump(dbg, f, indent=2)
    return dbg

def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--bsp", required=True)
    ap.add_argument("--template", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--debug", default=None)
    ap.add_argument("--style", type=int, default=0)
    args = ap.parse_args()
    info = convert_bspx_octree_to_ironwail_raw(args.bsp, args.template, args.out, args.debug, preferred_style=args.style)
    print(json.dumps(info, indent=2))

if __name__ == "__main__":
    main()
