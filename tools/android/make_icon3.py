"""Render the bowtie on a virtual 2400x1080 canvas using the UI's own
normalized geometry (no text/LED/buttons), then crop into a square icon."""
import math
from PIL import Image, ImageDraw, ImageFilter

VW, VH = 2400.0, 1080.0
cx, cy = VW * 0.5, VH * 0.46

def vgrad(w, h, top, bot):
    g = Image.new("RGB", (1, int(h)))
    for y in range(int(h)):
        t = y / max(1, h - 1)
        g.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(top, bot)))
    return g.resize((int(w), int(h)))

def cubic(p0, p1, p2, p3, n=48):
    pts = []
    for i in range(n + 1):
        t = i / n
        pts.append(((1-t)**3*p0[0] + 3*(1-t)**2*t*p1[0] + 3*(1-t)*t*t*p2[0] + t**3*p3[0],
                    (1-t)**3*p0[1] + 3*(1-t)**2*t*p1[1] + 3*(1-t)*t*t*p2[1] + t**3*p3[1]))
    return pts

def wing_pts(s):
    def X(o): return cx + s * o * VW
    def Y(o): return cy + o * VH
    kx, kxB, ox = 0.048, 0.085, 0.27
    pts = []
    pts += cubic((X(kx), Y(-0.165)), (X(0.09), Y(-0.25)),
                 (X(ox-0.05), Y(-0.29)), (X(ox-0.016), Y(-0.28)))
    pts += [(X(ox + 0.010), Y(-0.26)), (X(ox + 0.005), Y(-0.11))]
    pts += cubic((X(ox + 0.005), Y(-0.11)), (X(ox + 0.009), Y(0.04)),
                 (X(ox + 0.009), Y(0.07)), (X(ox), Y(0.20)), n=24)
    pts += cubic((X(ox), Y(0.20)), (X(ox-0.05), Y(0.285)),
                 (X(0.10), Y(0.22)), (X(kxB), Y(0.14)), n=24)
    pts += [(X(kx), Y(0.06))]
    return pts

img = Image.new("RGBA", (int(VW), int(VH)), (255, 255, 255, 255))

# wings
wing_grad = vgrad(VW, VH, (240, 64, 72), (177, 18, 24))
for m in (False, True):
    pts = wing_pts(-1 if m else 1)
    wm = Image.new("L", img.size, 0)
    ImageDraw.Draw(wm).polygon(pts, fill=255)
    img.paste(wing_grad, (0, 0), wm)
    gloss = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(gloss)
    gd.polygon(pts[:len(pts)//2 + 6], fill=(255, 255, 255, 34))
    gloss = gloss.filter(ImageFilter.GaussianBlur(8))
    img = Image.alpha_composite(img, gloss)

# module
mod_grad = vgrad(VW, VH, (228, 42, 50), (165, 15, 22))
mw, mh = 0.116 * VW, 0.39 * VH
rect = (cx - mw/2, cy - mh/2, cx + mw/2, cy + mh/2)
mm = Image.new("L", img.size, 0)
ImageDraw.Draw(mm).rounded_rectangle(rect, radius=mw * 0.34, fill=255)
img.paste(mod_grad, (0, 0), mm)
gl = Image.new("RGBA", img.size, (0, 0, 0, 0))
gd = ImageDraw.Draw(gl)
gd.rounded_rectangle((rect[0]+12, rect[1]+10, rect[2]-12, cy), mw*0.30,
                     fill=(255, 255, 255, 52))
gl = gl.filter(ImageFilter.GaussianBlur(2))
img = Image.alpha_composite(img, gl)

# dome button
dome_cy = cy - mh/2 + mh * 0.32
br = mw * 0.24
ring = Image.new("RGBA", img.size, (0, 0, 0, 0))
ImageDraw.Draw(ring).ellipse((cx-br-10, dome_cy-br-10, cx+br+10, dome_cy+br+10),
                             fill=(90, 30, 12, 255))
img = Image.alpha_composite(img, ring)
dm = Image.new("L", img.size, 0)
ImageDraw.Draw(dm).ellipse((cx-br, dome_cy-br, cx+br, dome_cy+br), fill=255)
dg = vgrad(VW, VH, (248, 105, 110), (179, 18, 24))
img.paste(dg, (0, 0), dm)
r = int(br)
hm = Image.new("L", (r*2, r*2), 0)
ImageDraw.Draw(hm).ellipse((0, 0, r*2, r*2), fill=255)     # circular vignette
hi = Image.radial_gradient("L").resize((r*2, r*2)).point(
    lambda v: int((255 - v) * 0.5))
hm.paste(hi, (0, 0), hm)
hil = Image.new("RGBA", (r*2, r*2), (255, 225, 225, 0))
hil.putalpha(hm)
img.paste(hil, (int(cx-br*0.78), int(dome_cy-br*0.9)), hil)

# ---- crop the bowtie bbox into a square icon ------------------------------
crop = img.crop((500, 165, 1910, 830))      # 1410 x 665, wings full
side = 1024
sq = Image.new("RGBA", (side, side), (255, 255, 255, 255))
tw = int(side * 0.86)
c2 = crop.resize((tw, int(crop.height * tw / crop.width)), Image.LANCZOS)
sq.paste(c2, ((side - tw)//2, (side - c2.height)//2 - 8), c2)
sq.convert("RGB").save("G:/tts/mean2vc/icon_square.png")

# adaptive foreground: transparent bg, bowtie inside the ~66% safe zone
fg = Image.new("RGBA", (side, side), (0, 0, 0, 0))
fw = int(side * 0.58)
c3 = crop.resize((fw, int(crop.height * fw / crop.width)), Image.LANCZOS)
fg.paste(c3, ((side - fw)//2, (side - c3.height)//2 - 6), c3)
fg.save("G:/tts/mean2vc/icon_fg.png")
print("written")
