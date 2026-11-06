"""Render the bowtie device as a clean app icon — same geometry/colors as
BowtieView (wings + module + dome), no UI text/chrome."""
import io, math
from PIL import Image, ImageDraw, ImageFilter

S = 1024
W = H = float(S)
cx, cy = W * 0.5, H * 0.52

def lerp(a, b, t): return a + (b - a) * t

def vgrad(w, h, top, bot):
    g = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        g.putpixel((0, y), (int(lerp(top[0], bot[0], t)),
                            int(lerp(top[1], bot[1], t)),
                            int(lerp(top[2], bot[2], t))))
    return g.resize((w, h))

def cubic(p0, p1, p2, p3, n=48):
    pts = []
    for i in range(n + 1):
        t = i / n
        x = (1-t)**3*p0[0] + 3*(1-t)**2*t*p1[0] + 3*(1-t)*t*t*p2[0] + t**3*p3[0]
        y = (1-t)**3*p0[1] + 3*(1-t)**2*t*p1[1] + 3*(1-t)*t*t*p2[1] + t**3*p3[1]
        pts.append((x, y))
    return pts

def wing_pts(s):
    def X(o): return cx + s * o * W
    def Y(o): return cy + o * H
    kx, ox = 0.048, 0.27
    pts = []
    pts += cubic((X(kx), Y(-0.165)), (X(0.09), Y(-0.25)),
                 (X(ox-0.05), Y(-0.29)), (X(ox-0.016), Y(-0.28)))
    pts += [(X(ox + 0.010), Y(-0.26)), (X(ox + 0.005), Y(-0.11))]
    pts += cubic((X(ox + 0.005), Y(-0.11)), (X(ox + 0.009), Y(0.04)),
                 (X(ox + 0.009), Y(0.07)), (X(ox), Y(0.20)), n=24)
    pts += cubic((X(ox), Y(0.20)), (X(ox-0.05), Y(0.285)),
                 (X(0.10), Y(0.22)), (X(kxB := 0.085), Y(0.14)), n=24)
    pts += [(X(kx), Y(0.06))]
    return pts

def draw_shape(img, pts, fill_src, mask_only=False):
    m = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(m)
    d.polygon(pts, fill=255)
    if mask_only:
        return m
    img.paste(fill_src, (0, 0), m)

def rounded_rect_mask(size, rect, radius):
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle(rect, radius=radius, fill=255)
    return m

def compose(with_bg):
    img = Image.new("RGBA", (S, S), (255, 255, 255, 255) if with_bg else (0, 0, 0, 0))
    # soft ground shadow
    if with_bg:
        sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        sh_a = Image.radial_gradient("L").resize((int(S*0.62), int(S*0.62)))
        sh_a = sh_a.point(lambda v: int((255 - v) * 0.28))
        shl = Image.new("RGBA", sh_a.size, (0, 0, 0, 0))
        shl.putalpha(sh_a)
        sh.paste(shl, (int(cx - S*0.31), int(cy + H*0.24 - S*0.31)), shl)
        img = Image.alpha_composite(img, sh)

    img = img.convert("RGBA")
    # wings
    wing_grad = vgrad(S, S, (240, 64, 72), (177, 18, 24))
    for m in (False, True):
        pts = wing_pts(-1 if m else 1)
        wm = Image.new("L", (S, S), 0)
        ImageDraw.Draw(wm).polygon(pts, fill=255)
        img.paste(wing_grad, (0, 0), wm)
        # top gloss: soft white wedge over the upper wing
        gloss = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        gd = ImageDraw.Draw(gloss)
        gd.polygon(pts[:len(pts)//2 + 6], fill=(255, 255, 255, 34))
        gloss = gloss.filter(ImageFilter.GaussianBlur(6))
        img = Image.alpha_composite(img, gloss)
    # module (rounded square) + gloss
    mod_grad = vgrad(S, S, (228, 42, 50), (165, 15, 22))
    mw, mh = 0.116 * W, 0.39 * H
    rect = (cx - mw/2, cy - mh/2, cx + mw/2, cy + mh/2)
    mmod = rounded_rect_mask((S, S), rect, mw * 0.34)
    img.paste(mod_grad, (0, 0), mmod)
    gl = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(gl)
    gd.rounded_rectangle((rect[0]+12, rect[1]+10, rect[2]-12, cy), mw*0.30,
                         fill=(255, 255, 255, 52))
    gl = gl.filter(ImageFilter.GaussianBlur(2))
    img = Image.alpha_composite(img, gl)

    # dome button
    dome_cy = cy - mh/2 + mh * 0.32
    br = mw * 0.24
    dome = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dome)
    dd.ellipse((cx-br-10, dome_cy-br-10, cx+br+10, dome_cy+br+10),
               fill=(90, 30, 12, 255))          # seam ring (dark red)
    dome.putalpha(dome.split()[3].point(lambda v: v))
    dm = Image.new("L", (S, S), 0)
    ImageDraw.Draw(dm).ellipse((cx-br, dome_cy-br, cx+br, dome_cy+br), fill=255)
    dg = vgrad(S, S, (248, 105, 110), (179, 18, 24))
    dome.paste(dg, (0, 0), dm)
    # dome radial highlight
    hi = Image.radial_gradient("L").resize((int(br*2), int(br*2)))
    hi = hi.point(lambda v: int((255 - v) * 0.5))
    hil = Image.new("RGBA", (int(br*2), int(br*2)), (255, 220, 220, 0))
    hil.putalpha(hi.point(lambda v: int(v * 0.55)))
    dome.paste(hil, (int(cx - br - br*0.25), int(dome_cy - br - br*0.3)), hil)
    img = Image.alpha_composite(img, dome)
    return img

square = compose(with_bg=True).convert("RGB")
square.save("G:/tts/mean2vc/icon_square.png")

fg = compose(with_bg=False)
fg.save("G:/tts/mean2vc/icon_fg.png")
print("icons written")
