#!/usr/bin/env python3
"""Generates the drone's model sources: a 5" freestyle FPV quad, made for this
mod (no outside assets). Writes SMD meshes, the QC, VMT materials and small
VTF textures into model/build/ and content/materials/models/fpv/.

Sizes follow libfpv's freestyler-5 preset (motors 0.079 m from the centre on
both axes, 5" props), in Source units (inches). Model space: X forward, Y left,
Z up, origin at the drone's centre (libfpv's frame.position).

Run model/build.sh, which also compiles it with studiomdl.
"""
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "build")
MATERIALS = os.path.join(HERE, "..", "content", "materials", "models", "fpv")

M = 39.37  # inches per metre
MOTOR = 0.079 * M  # 3.11
PROP_R = 0.0635 * M  # 2.5
PROP_Z = 0.62


class Mesh:
    def __init__(self):
        self.tris = []  # (material, [(pos, normal, uv) x3])

    def tri(self, mat, a, b, c, n=None, uvs=((0, 0), (1, 0), (1, 1))):
        if n is None:
            n = norm(cross(sub(b, a), sub(c, a)))
        self.tris.append((mat, [(a, n, uvs[0]), (b, n, uvs[1]), (c, n, uvs[2])]))

    def quad(self, mat, a, b, c, d, n=None):
        """a b c d counter-clockwise seen from the side the normal faces."""
        self.tri(mat, a, b, c, n, ((0, 0), (1, 0), (1, 1)))
        self.tri(mat, a, c, d, n, ((0, 0), (1, 1), (0, 1)))

    def box(self, mat, center, half, axes=None, side_mats=None):
        """An oriented box: half extents along its axes (default X, Y, Z)."""
        ax = axes or ((1, 0, 0), (0, 1, 0), (0, 0, 1))
        c = center

        def corner(sx, sy, sz):
            return add(c, add(scale(ax[0], sx * half[0]), add(scale(ax[1], sy * half[1]), scale(ax[2], sz * half[2]))))

        faces = [  # (normal sign axis, corners counter-clockwise from outside)
            ((0, 1), [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]),
            ((0, -1), [(-1, 1, -1), (-1, -1, -1), (-1, -1, 1), (-1, 1, 1)]),
            ((1, 1), [(1, 1, -1), (-1, 1, -1), (-1, 1, 1), (1, 1, 1)]),
            ((1, -1), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
            ((2, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
            ((2, -1), [(-1, 1, -1), (1, 1, -1), (1, -1, -1), (-1, -1, -1)]),
        ]
        for i, ((axis, sign), cs) in enumerate(faces):
            n = scale(ax[axis], sign)
            m = side_mats.get(i, mat) if side_mats else mat
            self.quad(m, *[corner(*k) for k in cs], n=n)

    def cylinder(self, mat, base, radius, height, segments=16, cap_mat=None, axis=(0, 0, 1)):
        u, v = basis(axis)
        top = add(base, scale(axis, height))
        for i in range(segments):
            a0, a1 = 2 * math.pi * i / segments, 2 * math.pi * (i + 1) / segments
            d0 = add(scale(u, math.cos(a0)), scale(v, math.sin(a0)))
            d1 = add(scale(u, math.cos(a1)), scale(v, math.sin(a1)))
            p0, p1 = add(base, scale(d0, radius)), add(base, scale(d1, radius))
            q0, q1 = add(top, scale(d0, radius)), add(top, scale(d1, radius))
            self.quad(mat, p0, p1, q1, q0, n=norm(add(d0, d1)))
            cm = cap_mat or mat
            self.tri(cm, top, q0, q1, n=axis)
            self.tri(cm, base, p1, p0, n=scale(axis, -1))

    def disc(self, mat, center, radius, segments=28):
        for i in range(segments):
            a0, a1 = 2 * math.pi * i / segments, 2 * math.pi * (i + 1) / segments
            p0 = add(center, (radius * math.cos(a0), radius * math.sin(a0), 0))
            p1 = add(center, (radius * math.cos(a1), radius * math.sin(a1), 0))
            uv = lambda a: (0.5 + 0.5 * math.cos(a), 0.5 + 0.5 * math.sin(a))
            self.tri(mat, center, p0, p1, n=(0, 0, 1), uvs=((0.5, 0.5), uv(a0), uv(a1)))

    def write(self, path):
        with open(path, "w") as f:
            f.write("version 1\nnodes\n0 \"root\" -1\nend\nskeleton\ntime 0\n0 0 0 0 0 0 0\nend\ntriangles\n")
            for mat, verts in self.tris:
                f.write(mat + "\n")
                for p, n, uv in verts:
                    f.write("0 %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n" % (p + n + uv))
            f.write("end\n")


def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def scale(a, k): return (a[0] * k, a[1] * k, a[2] * k)
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    l = math.sqrt(a[0] ** 2 + a[1] ** 2 + a[2] ** 2) or 1
    return scale(a, 1 / l)


def basis(axis):
    t = (1, 0, 0) if abs(axis[0]) < 0.9 else (0, 1, 0)
    u = norm(cross(axis, t))
    return u, cross(axis, u)


# motors: front left, front right, rear right, rear left (X forward, Y left)
MOTORS = [(MOTOR, MOTOR), (MOTOR, -MOTOR), (-MOTOR, -MOTOR), (-MOTOR, MOTOR)]


def body():
    m = Mesh()
    # X frame: four arms from the centre to the motors
    for (x, y) in MOTORS:
        d = norm((x, y, 0))
        length = math.hypot(x, y)
        m.box("carbon", (x / 2, y / 2, -0.55), (length / 2 + 0.3, 0.32, 0.1), axes=(d, (-d[1], d[0], 0), (0, 0, 1)))
    # the stack under the top plate, and the top plate
    m.box("electronics", (0, 0, -0.12), (0.95, 0.75, 0.35))
    m.box("carbon", (0, 0, 0.32), (1.5, 0.85, 0.07))
    for sx in (-1, 1):
        for sy in (-1, 1):
            m.cylinder("metal", (sx * 1.2, sy * 0.65, -0.45), 0.08, 0.72, 6)  # standoffs
    # the battery: Aperture white, a blue and an orange stripe, a black strap
    m.box("white", (-0.15, 0, 0.98), (1.45, 0.72, 0.58), side_mats={2: "blue", 3: "orange"})
    m.box("strap", (-0.15, 0, 0.98), (0.22, 0.76, 0.61))
    # the camera, tilted up 25 degrees, and its lens
    t = math.radians(25)
    fwd, up = (math.cos(t), 0, math.sin(t)), (-math.sin(t), 0, math.cos(t))
    m.box("camera", (1.45, 0, -0.05), (0.38, 0.4, 0.38), axes=(fwd, (0, 1, 0), up))
    m.cylinder("lens", add((1.45, 0, -0.05), scale(fwd, 0.38)), 0.26, 0.22, 14, axis=fwd)
    # antenna
    a = norm((-0.5, 0, 1))
    m.cylinder("white", (-1.45, 0, 0.35), 0.06, 1.3, 6, axis=a)
    # motors: base, bell, shaft
    for (x, y) in MOTORS:
        m.cylinder("metal", (x, y, -0.66), 0.5, 0.2, 16)
        m.cylinder("motor", (x, y, -0.46), 0.52, 0.72, 18, cap_mat="metal")
        m.cylinder("metal", (x, y, 0.26), 0.1, 0.42, 6)
    return m


def props_still():
    m = Mesh()
    for i, (x, y) in enumerate(MOTORS):
        a = math.radians(30 + 60 * i)
        d = (math.cos(a), math.sin(a), 0)
        m.box("prop", (x, y, PROP_Z), (PROP_R, 0.24, 0.035), axes=(d, (-d[1], d[0], 0), (0, 0, 1)))
        m.cylinder("prop", (x, y, PROP_Z - 0.06), 0.28, 0.16, 10)
    return m


def props_spin():
    m = Mesh()
    for (x, y) in MOTORS:
        m.disc("propblur", (x, y, PROP_Z), PROP_R)
        m.cylinder("prop", (x, y, PROP_Z - 0.06), 0.28, 0.16, 10)
    return m


# --- materials -----------------------------------------------------------------------------

COLORS = {  # name: (r, g, b, a) or a pattern function
    "carbon": (34, 34, 38, 255),
    "electronics": (24, 60, 32, 255),
    "metal": (150, 152, 158, 255),
    "motor": (70, 72, 80, 255),
    "white": (236, 236, 232, 255),
    "blue": (40, 140, 255, 255),
    "orange": (255, 140, 30, 255),
    "strap": (20, 20, 22, 255),
    "camera": (18, 18, 20, 255),
    "lens": (30, 40, 60, 255),
    "prop": (40, 40, 44, 255),
    "propblur": (60, 60, 66, 70),
}


def vtf(path, size, rgba):
    """An uncompressed single-mip VTF 7.2 (BGRA8888)."""
    w = h = size
    flags = 0x0100 | 0x0200 | (0x2000 if rgba[3] < 255 else 0)  # NOMIP, NOLOD, EIGHTBITALPHA
    hdr = struct.pack("<4sIIIHHIHH4x3f4xfIBIBBH", b"VTF\0", 7, 2, 80, w, h, flags, 1, 0,
                      rgba[0] / 255, rgba[1] / 255, rgba[2] / 255, 1.0, 12, 1, 0xFFFFFFFF, 0, 0, 1)
    hdr = hdr.ljust(80, b"\0")
    px = bytes((rgba[2], rgba[1], rgba[0], rgba[3])) * (w * h)
    with open(path, "wb") as f:
        f.write(hdr + px)


def vmt(path, name, rgba):
    lines = ['"VertexLitGeneric"', "{", '\t"$basetexture" "models/fpv/%s"' % name]
    if rgba[3] < 255:
        lines += ['\t"$translucent" "1"', '\t"$nocull" "1"']
    lines.append("}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


QC = """// portal2-fpv's drone: a 5" freestyle FPV quad (generated by build_model.py)
$modelname "fpv/quad5.mdl"
$cdmaterials "models/fpv"
$surfaceprop "plastic"
$scale 1
$body body "body.smd"
$bodygroup props
{
\tstudio "props_still.smd"
\tstudio "props_spin.smd"
}
$sequence idle "body.smd" fps 30
$illumposition 0 0 0
"""


def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(MATERIALS, exist_ok=True)
    body().write(os.path.join(OUT, "body.smd"))
    props_still().write(os.path.join(OUT, "props_still.smd"))
    props_spin().write(os.path.join(OUT, "props_spin.smd"))
    with open(os.path.join(OUT, "quad5.qc"), "w") as f:
        f.write(QC)
    for name, rgba in COLORS.items():
        vtf(os.path.join(MATERIALS, name + ".vtf"), 8, rgba)
        vmt(os.path.join(MATERIALS, name + ".vmt"), name, rgba)
    print("sources in", OUT, "materials in", os.path.normpath(MATERIALS))


if __name__ == "__main__":
    sys.exit(main())
