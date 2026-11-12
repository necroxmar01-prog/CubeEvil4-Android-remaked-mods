#!/usr/bin/env python3
"""Reads the PC release's weapon models (RE4 2007 / UHD .pmd) and draws a preview sheet.

First step of the weapon import (wish of DevJunior, 12.11.2026: a PC weapon pack in CubeEvil4). The format follows
JADERLINK's RE4-2007-PMD-TOOL (PmdDecoder.cs): a header text (58 bytes), the skeleton's and the node groups' names,
the skeleton (parent + 26 floats each: scale, rotation, 4 floats, the 4x4 matrix with the translation in the last
row), the objects (a skeleton bone, the texture index of each mesh, per mesh a triangle list of u16 indices and
vertices of 16 floats: x y z, weights w0 w1, bone slots i0 i1 (the object's bone list, 3 per entry), normal, uv,
colour rgba; then the object's bones), and the materials (17 floats, a flag, the .tga name). Little-endian.

    port/tools/pmd_preview.py OUT.png FILE.pmd [FILE.pmd ...]

Without the textures: a grey side view of each model (its longest axis across), its triangle count.
"""
import os
import struct
import sys


class R:
    def __init__(s, d): s.d, s.o = d, 0
    def u32(s): v = struct.unpack_from('<I', s.d, s.o)[0]; s.o += 4; return v
    def i32(s): v = struct.unpack_from('<i', s.d, s.o)[0]; s.o += 4; return v
    def f(s, n): v = struct.unpack_from('<%df' % n, s.d, s.o); s.o += 4 * n; return v
    def raw(s, n): v = s.d[s.o:s.o + n]; s.o += n; return v
    def names(s):
        n = s.u32(); out = [None] * n
        for _ in range(n):
            l = s.u32(); nm = s.raw(l).decode('ascii', 'replace'); out[s.u32()] = nm
        return out

def read(path):
    r = R(open(path, 'rb').read())
    r.raw(58)
    bones = r.names(); groups = r.names()
    r.u32()
    nb = r.u32(); skel = []
    for _ in range(nb):
        p = r.i32(); skel.append((p, r.f(26)))
    nodes = []
    for _ in range(r.u32()):
        si = r.i32(); r.raw(32)
        nm = r.u32(); tex = [r.i32() for _ in range(nm)]
        r.u32(); r.u32()
        meshes = []
        for im in range(nm):
            if r.u32() != 64:
                continue
            ni = r.u32(); idx = struct.unpack_from('<%dH' % ni, r.d, r.o); r.o += 2 * ni
            nv = r.u32(); vs = [struct.unpack_from('<16f', r.d, r.o + 64 * k) for k in range(nv)]; r.o += 64 * nv
            meshes.append((tex[im], idx, vs))
        nbn = r.u32(); nbones = []
        for _ in range(nbn):
            bid = r.i32(); r.raw(52); xyzw = r.f(4); nbones.append((bid, xyzw))
        nodes.append({'skel': si, 'meshes': meshes, 'bones': nbones})
    mats = []
    for _ in range(r.u32()):
        data = r.f(17); en = r.i32(); l = r.i32(); mats.append((r.raw(l).decode('ascii', 'replace'), en, data))
    assert r.o == len(r.d), (path, r.o, len(r.d))
    return {'bones': bones, 'groups': groups, 'skel': skel, 'nodes': nodes, 'mats': mats}


def sheet(out, paths):
    import numpy as np
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    cols = 3
    rows = (len(paths) + cols - 1) // cols
    fig, axs = plt.subplots(rows, cols, figsize=(12, 3 * rows), dpi=90, squeeze=False)
    for ax in axs.flat:
        ax.axis('off')
    for ax, path in zip(axs.flat, paths):
        m = read(path)
        parts = []
        for n in m['nodes']:
            for _tex, idx, vs in n['meshes']:
                v = np.array([x[:3] for x in vs])
                t = np.array(idx[:len(idx) // 3 * 3]).reshape(-1, 3)
                parts.append(v[t])
        tris = np.concatenate(parts)
        ext = tris.reshape(-1, 3).max(0) - tris.reshape(-1, 3).min(0)
        p = tris[:, :, np.argsort(-ext)]
        nrm = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
        shade = np.abs(nrm @ np.array([0.35, 0.6, 0.72])) / (np.linalg.norm(nrm, axis=1) + 1e-12)
        k = np.argsort(p[:, :, 2].mean(1))
        ax.add_collection(PolyCollection(p[k][:, :, :2], facecolors=plt.cm.Greys_r(0.15 + 0.8 * shade[k]),
                                         edgecolors='none'))
        ax.autoscale()
        ax.set_aspect('equal')
        ax.set_title('%s (%s), %d triangles' % (os.path.basename(path), m['bones'][1] if len(m['bones']) > 1 else '',
                                                len(tris)), fontsize=9)
    plt.tight_layout()
    plt.savefig(out)


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sheet(sys.argv[1], sys.argv[2:])
