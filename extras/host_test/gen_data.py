import math, os, struct, shutil, subprocess, sys
from PIL import Image, ImageDraw

CLAT, CLON = 37.7749, -122.4194
OUT = "/tmp/maptest"
shutil.rmtree(OUT, ignore_errors=True)
PNG = OUT + "/png"; os.makedirs(PNG)

def wpx(lat, lon, z):
    n = 256 * (1 << z)
    x = (lon + 180) / 360 * n
    r = math.radians(lat)
    y = (1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * n
    return x, y

def off(lat, lon, dn, de):   # metres north/east
    return lat + dn / 111320.0, lon + de / (111320.0 * math.cos(math.radians(lat)))

# ---- route: ~1.6 km, east, north, east, north (an S-ish path), points every ~25 m
corners = [(0, -700), (0, 0), (500, 0), (500, 600), (900, 600)]   # (north, east) m from centre
pts = []
for (n0, e0), (n1, e1) in zip(corners, corners[1:]):
    d = math.hypot(n1 - n0, e1 - e0); k = max(1, int(d / 25))
    for i in range(k):
        pts.append(off(CLAT, CLON, n0 + (n1 - n0) * i / k, e0 + (e1 - e0) * i / k))
pts.append(off(CLAT, CLON, *corners[-1]))

def hav(a, b):
    p1, p2 = math.radians(a[0]), math.radians(b[0]); dl = math.radians(b[1] - a[1])
    h = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * 6371008.8 * math.asin(math.sqrt(h))
def bear(a, b):
    p1, p2 = math.radians(a[0]), math.radians(b[0]); dl = math.radians(b[1] - a[1])
    y = math.sin(dl) * math.cos(p2); x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return (math.degrees(math.atan2(y, x)) + 360) % 360

cum = [0.0]
for a, b in zip(pts, pts[1:]): cum.append(cum[-1] + hav(a, b))
total = int(round(cum[-1]))

# ---- tiles with the road drawn at the SAME world coordinates as the route
sets = {15: 2, 16: 3, 17: 4}          # half-width in tiles around the centre tile
for z, hw in sets.items():
    cx, cy = [int(v // 256) for v in wpx(CLAT, CLON, z)]
    road = [wpx(la, lo, z) for la, lo in pts]
    for tx in range(cx - hw, cx + hw + 1):
        for ty in range(cy - hw, cy + hw + 1):
            im = Image.new("RGB", (256, 256), (232, 228, 221) if (tx + ty) % 2 else (214, 222, 210))
            d = ImageDraw.Draw(im)
            for g in range(0, 256, 64):
                d.line([(g, 0), (g, 255)], fill=(200, 200, 200)); d.line([(0, g), (255, g)], fill=(200, 200, 200))
            d.line([(x - tx * 256, y - ty * 256) for x, y in road], fill=(120, 120, 120), width=max(6, 4 + (z - 14) * 2), joint="curve")
            d.rectangle([0, 0, 255, 255], outline=(255, 0, 0))
            d.text((8, 8), f"z{z} {tx},{ty}", fill=(0, 0, 0))
            p = f"{PNG}/{z}/{tx}"; os.makedirs(p, exist_ok=True); im.save(f"{p}/{ty}.png")

subprocess.check_call([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts", "lvgl_map_tile_converter.py"),
                       "-i", PNG, "-o", OUT + "/tiles1", "-j", "8"], stdout=subprocess.DEVNULL)
shutil.rmtree(PNG)

# ---- route.bin (MRT1)
strings = bytearray(b"\0")
def s(txt):
    off_ = len(strings); strings.extend(txt.encode() + b"\0"); return off_
name_off = s("Test loop"); folder_off = s("tiles1")
man = []
def maneuver(idx, typ, street, instr, mod=0):
    man.append((idx, int(cum[idx]), s(street), s(instr), typ, mod, 0))
# DEPART=1 ARRIVE=2 TURN_LEFT=5 TURN_RIGHT=8
corner_idx = []
acc = 0
for (n0, e0), (n1, e1) in zip(corners, corners[1:]):
    acc += max(1, int(math.hypot(n1 - n0, e1 - e0) / 25)); corner_idx.append(acc)
maneuver(0, 1, "Start St", "Head east")
maneuver(corner_idx[0], 5, "North Ave", "Turn left onto North Ave")
maneuver(corner_idx[1], 8, "East Rd", "Turn right onto East Rd")
maneuver(corner_idx[2], 5, "Final Way", "Turn left onto Final Way")
maneuver(len(pts) - 1, 2, "", "Arrive")

lats = [p[0] for p in pts]; lons = [p[1] for p in pts]
e7 = lambda v: int(round(v * 1e7))
hdr = struct.pack("<4sHHIIIIIiiiiIBBHBBHII", b"MRT1", 1, 64, len(pts), len(man), len(strings), total, total // 8,
                  e7(min(lats)), e7(min(lons)), e7(max(lats)), e7(max(lons)), name_off, 15, 17, 0, 0, 0, 0, folder_off, 0)
assert len(hdr) == 64
body = bytearray()
for i, (la, lo) in enumerate(pts):
    bc = 0xFFFF if i == len(pts) - 1 else int(bear(pts[i], pts[i + 1]) * 100) % 36000
    body += struct.pack("<iiIBBH", e7(la), e7(lo), int(cum[i]), 0 if i % 4 == 0 else 16, 1 if any(m[0] == i for m in man) else 0, bc)
for m in man: body += struct.pack("<IIIIBBH", *m)
os.makedirs(OUT + "/routes", exist_ok=True)
open(OUT + "/routes/route.bin", "wb").write(hdr + bytes(body) + bytes(strings))
print("route points", len(pts), "length", total, "m; tiles:", sum(len(f) for _, _, f in os.walk(OUT + "/tiles1")))
cx, cy = [int(v // 256) for v in wpx(CLAT, CLON, 16)]; print("centre tile z16", cx, cy)

# ---- far route: two points ~15 km apart, to prove far-off-screen segments do not wrap in int16
a = off(CLAT, CLON, 0, -7500); b = off(CLAT, CLON, 0, 7500)
fp = [a, b]; d = int(hav(a, b))
fh = struct.pack("<4sHHIIIIIiiiiIBBHBBHII", b"MRT1", 1, 64, 2, 0, 1, d, d // 10,
                 e7(a[0]), e7(a[1]), e7(b[0]), e7(b[1]), 0xFFFFFFFF, 0, 22, 0, 0, 0, 0, 0xFFFFFFFF, 0)
fb = b"".join(struct.pack("<iiIBBH", e7(p[0]), e7(p[1]), c, 0, 0, 0x5A00 if i == 0 else 0xFFFF) for i, (p, c) in enumerate(zip(fp, (0, d))))
open(OUT + "/routes/far.bin", "wb").write(fh + fb + b"\0")

# ---- flat-colour tiles (z17), to measure seams between rotated tiles
FLAT_PNG = OUT + "/png_flat"
cx, cy = [int(v // 256) for v in wpx(CLAT, CLON, 17)]
for tx in range(cx - 4, cx + 5):
    for ty in range(cy - 4, cy + 5):
        p = f"{FLAT_PNG}/17/{tx}"; os.makedirs(p, exist_ok=True)
        Image.new("RGB", (256, 256), (120, 160, 200)).save(f"{p}/{ty}.png")
subprocess.check_call([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts", "lvgl_map_tile_converter.py"),
                       "-i", FLAT_PNG, "-o", OUT + "/tiles_flat", "-j", "8"], stdout=subprocess.DEVNULL)
shutil.rmtree(FLAT_PNG)
