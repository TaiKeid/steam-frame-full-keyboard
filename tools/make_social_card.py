#!/usr/bin/env python3
"""Draws the repository's social card, 1280x640, from the README's keyboard picture.

    python3 tools/make_social_card.py

Writes social-card.png (with the laser), social-card-no-laser.png and
keyboard-tilted.png (the keyboard alone, transparent, for the README) to
out/social-card/; working files go to its work/ folder. Copy
keyboard-tilted.png to docs/images/ for the README; the social card is
uploaded by hand in the repository's settings. Needs ImageMagick,
rsvg-convert and the Noto Sans font. Colours are the Graphite theme's.
"""
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parent.parent
here = root / "out/social-card"
work = here / "work"
picture = root / "docs/images/keyboard-graphite.png"
W, H = 1280, 640
CASE = (1600, 588)                     # the case in the picture, without the line below it
K_KEY = (704, 350, 760, 414)           # the key the laser points at
# Where the case's corners land: top edge farther away, so shorter.
# GitHub's own header shows only the middle 480 rows, so everything stays
# inside the template's border: 80 to 1200 across, 80 to 560 down.
CORNERS = [(178, 230), (1102, 230), (86, 538), (1194, 538)]
FEATURES = ["Swipe typing", "Dictation", "Emoji", "47 languages"]
LEGEND, DIM, SOFT, DOT = "#ffffff", "#aab4bf", "#c3ccd5", "#6f8ea6"


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def homography(src, dst):
    """The map that takes the four src points to the four dst points."""
    m = []
    for (x, y), (u, v) in zip(src, dst):
        m.append([x, y, 1, 0, 0, 0, -u * x, -u * y, u])
        m.append([0, 0, 0, x, y, 1, -v * x, -v * y, v])
    for c in range(8):
        pivot = max(range(c, 8), key=lambda r: abs(m[r][c]))
        m[c], m[pivot] = m[pivot], m[c]
        for r in range(8):
            if r != c:
                f = m[r][c] / m[c][c]
                m[r] = [a - f * b for a, b in zip(m[r], m[c])]
    h = [m[r][8] / m[r][r] for r in range(8)]
    return lambda x, y: ((h[0] * x + h[1] * y + h[2]) / (h[6] * x + h[7] * y + 1),
                         (h[3] * x + h[4] * y + h[5]) / (h[6] * x + h[7] * y + 1))


def flat_pictures():
    """The case alone, with a light line along its top edge; and one with the K key hovered."""
    cw, ch = CASE
    (work / "rim.svg").write_text(f"""<svg xmlns="http://www.w3.org/2000/svg" width="{cw}" height="{ch}">
<defs><linearGradient id="r" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff" stop-opacity=".2"/><stop offset=".22" stop-color="#ffffff" stop-opacity="0"/></linearGradient></defs>
<rect x="1" y="1" width="{cw - 2}" height="{ch - 2}" rx="23" fill="none" stroke="url(#r)" stroke-width="2"/></svg>""")
    run("rsvg-convert", work / "rim.svg", "-o", work / "rim.png")
    run("magick", picture, "-crop", f"{cw}x{ch}+0+0", "+repage", work / "rim.png", "-composite", work / "case.png")
    # "lighten" raises the key's face to the hover colour and leaves its white legend alone.
    run("magick", work / "case.png", "(", "-size", f"{cw}x{ch}", "xc:none", "-fill", "#3d4855", "-draw",
        "roundrectangle {},{} {},{} 8,8".format(*K_KEY), ")", "-compose", "lighten", "-composite",
        work / "case-hover.png")


def tilt(source, target, corners=CORNERS, size=(W, H)):
    cw, ch = CASE
    points = " ".join(f"{sx},{sy} {dx * 2},{dy * 2}" for (sx, sy), (dx, dy) in
                      zip([(0, 0), (cw * 2, 0), (0, ch * 2), (cw * 2, ch * 2)], corners))
    # Twice the size and back, for smooth edges.
    run("magick", source, "-resize", "200%", "-alpha", "set", "-virtual-pixel", "transparent", "-define",
        f"distort:viewport={size[0] * 2}x{size[1] * 2}+0+0", "-distort", "Perspective", points, "-resize", "50%",
        target)


def laser():
    cw, ch = CASE
    at = homography([(0, 0), (cw, 0), (0, ch), (cw, ch)], CORNERS)
    # A little below and right of the legend, so the letter stays readable.
    x, y = at((K_KEY[0] + K_KEY[2]) / 2 + 13, (K_KEY[1] + K_KEY[3]) / 2 + 15)
    ox, oy = 1310, 720                 # the controller, off the card
    return f"""<defs>
<linearGradient id="beam" gradientUnits="userSpaceOnUse" x1="{x}" y1="{y}" x2="{ox}" y2="{oy}"><stop offset="0" stop-color="#cfe0ee" stop-opacity=".95"/><stop offset=".5" stop-color="#9fbdd4" stop-opacity=".6"/><stop offset="1" stop-color="#9fbdd4" stop-opacity=".25"/></linearGradient>
<radialGradient id="dot"><stop offset="0" stop-color="#ffffff"/><stop offset=".3" stop-color="#cfe0ee" stop-opacity=".7"/><stop offset="1" stop-color="#355b7a" stop-opacity="0"/></radialGradient>
<filter id="soft"><feGaussianBlur stdDeviation="5"/></filter></defs>
<line x1="{x}" y1="{y}" x2="{ox}" y2="{oy}" stroke="#6fa3c9" stroke-width="9" opacity=".35" filter="url(#soft)" stroke-linecap="round"/>
<line x1="{x}" y1="{y}" x2="{ox}" y2="{oy}" stroke="url(#beam)" stroke-width="2.6" stroke-linecap="round"/>
<circle cx="{x}" cy="{y}" r="13" fill="url(#dot)"/>
<circle cx="{x}" cy="{y}" r="3.4" fill="#ffffff"/>"""


def card(name, flat, with_laser):
    tilt(work / flat, work / f"{name}-tilt.png")
    dot = f'<tspan fill="{DOT}" dx="16">·</tspan>'
    features = dot.join(f'<tspan{" dx=\"16\"" if k else ""}>{word}</tspan>' for k, word in enumerate(FEATURES))
    (work / f"{name}.svg").write_text(f"""<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="'Noto Sans', sans-serif">
<defs>
<linearGradient id="back" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1b232c"/><stop offset="1" stop-color="#10161d"/></linearGradient>
<radialGradient id="glow" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#355b7a" stop-opacity=".62"/><stop offset=".55" stop-color="#355b7a" stop-opacity=".2"/><stop offset="1" stop-color="#355b7a" stop-opacity="0"/></radialGradient>
<radialGradient id="vignette" cx=".5" cy=".5" r=".75"><stop offset=".55" stop-color="#080c11" stop-opacity="0"/><stop offset="1" stop-color="#080c11" stop-opacity=".55"/></radialGradient>
<filter id="shadow" x="-20%" y="-20%" width="140%" height="160%"><feGaussianBlur in="SourceAlpha" stdDeviation="20"/><feOffset dy="22"/><feComponentTransfer><feFuncA type="linear" slope=".7"/></feComponentTransfer><feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge></filter>
</defs>
<rect width="{W}" height="{H}" fill="url(#back)"/>
<ellipse cx="640" cy="380" rx="840" ry="330" fill="url(#glow)"/>
<rect width="{W}" height="{H}" fill="url(#vignette)"/>
<text x="640" y="152" text-anchor="middle" font-size="66" letter-spacing="-.5" font-weight="700" fill="{LEGEND}">Full Keyboard <tspan font-weight="300" fill="{DIM}">for Steam Frame</tspan></text>
<text x="640" y="199" text-anchor="middle" font-size="31" font-weight="500" fill="{SOFT}">{features}</text>
<image xlink:href="{name}-tilt.png" x="0" y="0" width="{W}" height="{H}" filter="url(#shadow)"/>
{laser() if with_laser else ""}
</svg>""")
    run("rsvg-convert", work / f"{name}.svg", "-o", work / f"{name}-clean.png")
    # Fine grain keeps the dark gradients from banding once a site recompresses the card.
    run("magick", work / f"{name}-clean.png", "(", "-size", f"{W}x{H}", "xc:gray50", "-seed", "7", "-attenuate", ".2",
        "+noise", "Gaussian", "-colorspace", "Gray", ")", "-compose", "overlay", "-composite", "-alpha", "off",
        "-define", "png:compression-level=9", here / f"{name}.png")


def readme_picture(name):
    """The tilted keyboard alone on a transparent background, for the README.

    One picture has to sit on a white page and on a near-black one, so the
    halo is a blue-grey that reads as a soft shadow on white and as a glow on
    dark; the black shadow under it only shows on white."""
    w, h = 1600, 620
    corners = [(224, 104), (1376, 104), (110, 488), (1490, 488)]
    tilt(work / "case.png", work / f"{name}-tilt.png", corners, (w, h))
    (work / f"{name}.svg").write_text(f"""<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{w}" height="{h}" viewBox="0 0 {w} {h}">
<defs>
<filter id="halo" x="0" y="0" width="100%" height="100%" color-interpolation-filters="sRGB"><feGaussianBlur in="SourceAlpha" stdDeviation="40"/><feOffset dy="4"/><feComponentTransfer result="spread"><feFuncA type="linear" slope=".8"/></feComponentTransfer><feFlood flood-color="#3f6c90"/><feComposite operator="in" in2="spread"/></filter>
<filter id="drop" x="0" y="0" width="100%" height="100%" color-interpolation-filters="sRGB"><feGaussianBlur in="SourceAlpha" stdDeviation="18"/><feOffset dy="26"/><feComponentTransfer><feFuncA type="linear" slope=".45"/></feComponentTransfer></filter>
</defs>
<image xlink:href="{name}-tilt.png" width="{w}" height="{h}" filter="url(#halo)"/>
<image xlink:href="{name}-tilt.png" width="{w}" height="{h}" filter="url(#drop)"/>
<image xlink:href="{name}-tilt.png" width="{w}" height="{h}"/>
</svg>""")
    run("rsvg-convert", work / f"{name}.svg", "-o", here / f"{name}.png")


work.mkdir(parents=True, exist_ok=True)
flat_pictures()
card("social-card", "case-hover.png", True)
card("social-card-no-laser", "case.png", False)
readme_picture("keyboard-tilted")
