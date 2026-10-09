#!/usr/bin/env python3
"""Reproducibly convert the pinned SU2 tutorial output. Does not run a solver."""
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1] / 'Content/Samples/SU2_NACA0012'
SOURCE = ROOT / 'source'
manifest = json.loads((ROOT / 'provenance.json').read_text())
for name, digest in manifest['sha256'].items():
    assert hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() == digest, name
lines = (SOURCE / 'unsteady_naca0012_mesh.su2').read_text().splitlines()

def section(key):
    i = next(i for i, line in enumerate(lines) if line.startswith(key + '='))
    n = int(lines[i].split('=')[1].split()[0])
    return lines[i+1:i+1+n]

nodes = [tuple(map(float, line.split()[:2])) for line in section('NPOIN')]
triangles = []
for line in section('NELEM'):
    ids = list(map(int, line.split()))
    assert ids[0] == 9, 'Expected quadrilateral source mesh'
    a, b, c, d = ids[1:5]
    triangles.extend([(a,b,c), (a,c,d)])
i = next(i for i,line in enumerate(lines) if line.strip() == 'MARKER_TAG= airfoil')
n = int(lines[i+1].split('=')[1])
edges = [tuple(map(int,line.split()[1:3])) for line in lines[i+2:i+2+n]]
# Order the exact source boundary into a closed polygon.
adj = {}
for a,b in edges:
    adj.setdefault(a,[]).append(b)
    adj.setdefault(b,[]).append(a)
assert all(len(v)==2 for v in adj.values())
boundary = [edges[0][0]]
previous = None
while len(boundary)<n:
    current = boundary[-1]
    following = next(v for v in adj[current] if v != previous)
    assert following not in boundary
    boundary.append(following)
    previous = current
assert boundary[0] in adj[boundary[-1]]
frames = []
for index in (497,498,499):
    data = (SOURCE / f'restart_flow_{index:05d}.dat').read_bytes()
    magic,nfields,npoints,_,_ = struct.unpack_from('<5i',data)
    assert (magic,nfields,npoints)==(535532,17,len(nodes))
    fields = [data[20+j*33:20+(j+1)*33].split(b'\0')[0].decode() for j in range(nfields)]
    offset=20+nfields*33
    assert len(data)==offset+npoints*nfields*8
    rows = list(struct.iter_unpack('<17d',data[offset:]))
    frame=[]
    for point,row in zip(nodes,rows):
        assert abs(point[0]-row[0])<1e-10 and abs(point[1]-row[1])<1e-10
        rho = row[fields.index('Density')]
        assert rho>0
        frame.append((row[fields.index('Momentum_x')]/rho, row[fields.index('Momentum_y')]/rho,
                      row[fields.index('Pressure')], rho))
    frames.append((index,frame))
# Positions and velocities remain in original SU2 coordinates and units.
with (ROOT/'flow.bin').open('wb') as out:
    out.write(struct.pack('<6i',0x53553246,1,len(nodes),len(triangles),len(boundary),len(frames)))
    for point in nodes: out.write(struct.pack('<2d',*point))
    for tri in triangles: out.write(struct.pack('<3i',*tri))
    for node in boundary: out.write(struct.pack('<i',node))
    for index,frame in frames:
        out.write(struct.pack('<id',index,index*0.0005))
        for row in frame: out.write(struct.pack('<4d',*row))
print(f'Imported {len(nodes)} original nodes, {len(triangles)} triangles, {len(frames)} actual frames')
print('First source node x,y / velocity x,y / pressure / density:', nodes[0], frames[0][1][0])
