#!/usr/bin/env python3
"""Draws icon/settings_gear.png: the reNut pause tab's icon, a steel cog in the
pause icons' 3/4 view (ray-marched, 384x384 RGBA, dark outline like theirs).
make_menu_icon.py turns it into the texture.

usage: python render_settings_gear.py <out.png>
"""
import sys

import numpy as np
from PIL import Image, ImageFilter
N = 384
TEETH = 10
R_OUT, R_ROOT, R_HUB_HOLE = 0.78, 0.62, 0.20
THICK = 0.13
TILT = np.radians(52)   # towards the viewer
SPIN = np.radians(8)

def profile_sdf(x, y):
    r = np.hypot(x, y)
    th = np.arctan2(y, x) + SPIN
    # Rounded trapezoid teeth: a smoothed square wave of the angle.
    s = np.cos(th * TEETH)
    tooth = np.clip((s + 0.15) * 3.0, 0.0, 1.0)
    rad = R_ROOT + (R_OUT - R_ROOT) * tooth
    d = r - rad
    d = np.maximum(d, R_HUB_HOLE - r)
    # Six lightening holes in the web.
    for k in range(6):
        a = k * np.pi / 3 + SPIN
        cx, cy = 0.41 * np.cos(a), 0.41 * np.sin(a)
        d = np.maximum(d, 0.085 - np.hypot(x - cx, y - cy))
    return d * 0.8

def sdf(p):
    x, y, z = p[..., 0], p[..., 1], p[..., 2]
    d2 = profile_sdf(x, y)
    dz = np.abs(z) - THICK
    # Rounded edge (bevel).
    b = 0.025
    outside = np.hypot(np.maximum(d2 + b, 0), np.maximum(dz + b, 0))
    inside = np.minimum(np.maximum(d2 + b, dz + b), 0)
    return outside + inside - b

# Camera: orthographic, object tilted about x.
c, s = np.cos(TILT), np.sin(TILT)
rot = np.array([[1, 0, 0], [0, c, -s], [0, s, c]])  # world -> object is rot.T
u = (np.arange(N) + 0.5) / N * 1.86 - 0.93
X, Y = np.meshgrid(u, -u)
origin = np.stack([X, Y - 0.08, np.full_like(X, -3.0)], -1)
direction = np.array([0.0, 0.0, 1.0])
o = origin @ rot      # into object space
dv = direction @ rot
t = np.zeros(X.shape)
hit = np.zeros(X.shape, bool)
for _ in range(600):
    p = o + t[..., None] * dv
    d = sdf(p)
    hit |= d < 1e-3
    t = np.where(hit, t, t + np.maximum(d * 0.3, 5e-4))
p = o + t[..., None] * dv
e = 4e-3
n = np.stack([sdf(p + [e, 0, 0]) - sdf(p - [e, 0, 0]),
              sdf(p + [0, e, 0]) - sdf(p - [0, e, 0]),
              sdf(p + [0, 0, e]) - sdf(p - [0, 0, e])], -1)
n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9)
nw = n @ rot.T        # back to view space
light = np.array([-0.45, 0.65, -0.62]); light /= np.linalg.norm(light)
view = np.array([0.0, 0.0, -1.0])
diff = np.clip((nw * light).sum(-1), 0, 1)
h = light + view; h /= np.linalg.norm(h)
spec = np.clip((nw * h).sum(-1), 0, 1) ** 40
rim = (1 - np.clip(-(nw * view).sum(-1), 0, 1)) ** 3
# Steel with the game's soft cartoon steps: two light bands.
steel = np.array([0.70, 0.74, 0.80])
shade = 0.32 + 0.55 * np.round(diff * 3.0) / 3.0 * 0.6 + 0.4 * diff
col = steel * shade[..., None] + 0.55 * spec[..., None] + 0.12 * rim[..., None] * np.array([0.6, 0.7, 1.0])
# Faces facing the viewer slightly warmer (like the icons' lit tops).
top = np.clip(-(nw * view).sum(-1), 0, 1)
col = col * (0.92 + 0.08 * top[..., None])
col = col.clip(0, 1)
alpha = hit.astype(np.float32)
img = np.dstack([col, alpha])
im = Image.fromarray((img * 255).astype(np.uint8), 'RGBA')
# Dark outline as the icons have: the silhouette grown a little, behind.
a = Image.fromarray((alpha * 255).astype(np.uint8), 'L')
outline = a.filter(ImageFilter.MaxFilter(13)).filter(ImageFilter.GaussianBlur(1.2))
base = Image.new('RGBA', (N, N), (30, 32, 44, 0)); base.putalpha(outline)
base.alpha_composite(im)
base.save(sys.argv[1])
