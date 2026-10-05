#!/usr/bin/env python3
"""EffectForce skin polish (PolyForce's, unchanged: the meters part is unused here): redraws, after sd88me's generator (gen_vst.py) has written the skin, the images its C
renderer can't draw the way the design wants. `make skin` runs it right after gen_vst.py:

    skin_polish.py "<skin folder>/Plugin Skins" --layout surface/layout.conf --style surface/build/skin_style.json

1. Knob filmstrips sh_knob_r<r>.png: a 270-degree track, a value arc (from the minimum, or from 12 o'clock for
   the bipolar radii in skin_style.json), a two-tone face and a line pointer, 4x supersampled. Same size and
   frame count as shadow_art's strip() (128 frames of (2r+10)^2 stacked vertically, frame k = value k/127).
2. Trigger buttons sh_btn_<key>_<label>_{on,off}.png: the generator bakes a 36 px block (its label is " " when
   SHADOW_TITLE_FONT is set) into a wider image. Repainted at the full image size: a rounded button with a ring
   and the label from layout.conf; the primary buttons of skin_style.json in the accent colour.
3. Stepper arrows sh_arrow_<tab>_<key>_{prev,next}.png: shadow_skin crops them from the canvas as it stands
   after the LAST page mode was drawn, so a stepper inside a when= panel gets the wrong pixels. Redrawn: the
   h x h box rounded 5 of render_conf_preview.c's widget_stepper() with the design's centred triangle.
4. Wave view columns sh_meter_<w>x<h>.png (a look-less meter, drawn by shadow_art as a slider strip: PolyForce
   patch 5): 128 frames of max(w,h)^2, transparent around the w x h column; frame k is a bar from the zero line
   for the value k/127 (up above 0.5, down below), the zero line across the full width.
5. Sub-pages: the layout has one [tab] per page, and the generator gives each its own button in MPC's tab strip.
   TUI.json's tabs and the Q-Links files are renumbered so the pages of each group in skin_style.json
   ("tab_groups") are one button's sub-pages (a tap on the button again shows the next), as AIR DrumSynthMulti's
   stock skin numbers them.

The file names and sizes stay (TUI.json names them). Everything is checked first (the files exist, TUI.json uses
them, the sizes are the generator's, no unknown button/arrow/knob images, the tabs are the groups' pages) and
nothing is written unless all of it passes; a failure exits non-zero with the reason.

    skin_polish.py --selftest [--samples DIR]   fabricates a skin folder for the real layout, polishes it, checks
                                                 the results and the refusals; --samples also writes sample PNGs
"""
import argparse
import json
import math
import os
import re
import shlex
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SS = 4                 # supersampling factor
BUTTON_TEXT_PX = 16    # shadow_skin: int(39 * 0.42)
ARROW_RADIUS = 5       # render_conf_preview.c widget_stepper(): fill_rr(..., 5, ...)


class PolishError(Exception):
    pass


def _pil():
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        raise PolishError("Pillow is needed (the same Python as gen_vst.py)")
    return Image, ImageDraw, ImageFont


def _resample(Image, name):
    return getattr(getattr(Image, "Resampling", Image), name)


def _font(path, px):
    """The label font, with real kerning (raqm) where Pillow has it."""
    _, _, ImageFont = _pil()
    try:
        from PIL import features
        if features.check_feature("raqm"):
            f = ImageFont.truetype(path, px, layout_engine=ImageFont.Layout.RAQM)
            f.getbbox("Ag")   # raqm present but unusable (no fribidi) fails here, not mid-polish
            return f
    except Exception:   # an old Pillow without features/Layout, or no raqm: the basic layout below
        pass
    return ImageFont.truetype(path, px)


def pixels(im):
    im = im.convert("RGB")
    raw = im.tobytes()
    return [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]


def rgb(hexcol):
    return tuple(int(hexcol[i:i + 2], 16) for i in (0, 2, 4))


def shade(hexcol, f):   # shadow_skin.shade(): a button's "on" fill
    return "%02x%02x%02x" % tuple(max(0, min(255, int(c * f))) for c in rgb(hexcol))


def slug(t):   # shadow_skin.slug(): the label part of a button's image name
    return "".join(c if c.isalnum() else "_" for c in t).strip("_") or "x"


# --- the layout, read the way shadow_skin.parse_layout() reads it ---------------------------------
def parse_layout(path):
    """(top-level {key: value}, [tab {name, widgets}]) in file order."""
    top, tabs = {}, []
    for raw in open(path, encoding="utf-8"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"\[tab (.+)\]$", line)
        if m:
            tabs.append({"name": m.group(1).strip(), "widgets": []})
            continue
        if not tabs:
            k, eq, v = line.partition("=")
            if eq:
                top[k.strip()] = v.strip()
            continue
        if line.startswith("qlinks"):
            continue
        toks = shlex.split(line)
        w = {"kind": toks[0]}
        for t in toks[1:]:
            k, _, v = t.partition("=")
            w[k] = int(v) if k in ("cx", "cy", "w", "h", "r") else v
        tabs[-1]["widgets"].append(w)
    return top, tabs


class Skin:
    """What the generator wrote for this layout: knob radii, buttons, stepper arrows, with their sizes."""

    def __init__(self, layout_path, style):
        self.top, self.tabs = parse_layout(layout_path)
        self.base = os.path.dirname(os.path.abspath(layout_path))
        self.style = style
        self.td3 = self.top.get("style") == "td3"
        pal = style["palette"]
        self.under = pal["box"] if self.td3 else pal["bg"]   # shadow_skin under()
        self.knobs = sorted({w["r"] for t in self.tabs for w in t["widgets"] if w["kind"] == "knob"})
        self.meters = sorted({(w["w"], w["h"]) for t in self.tabs for w in t["widgets"] if w["kind"] == "meter"})
        self.buttons = {}   # image stem -> (key, label)
        self.arrows = {}    # file name -> (side, h)
        for t, tab in enumerate(self.tabs):
            for w in tab["widgets"]:
                if w["kind"] == "button":
                    if not w.get("label"):
                        raise PolishError("layout: button %s has no label" % w.get("key"))
                    self.buttons["sh_btn_%s_%s" % (w["key"], slug(w["label"]))] = (w["key"], w["label"])
                elif w["kind"] == "stepper":
                    for side in ("prev", "next"):
                        self.arrows["sh_arrow_%d_%s_%s.png" % (t, w["key"], side)] = (side, w["h"])

    def font(self, rel):
        path = os.path.join(self.base, rel)
        if not os.path.isfile(path):
            raise PolishError("font %s: no such file" % path)
        return path

    def button_size(self, label):
        """shadow_skin button_rect(): text_width() with font_label=, + 36; td3 + 28 wide and 52 tall."""
        _, _, ImageFont = _pil()
        if self.top.get("font_label"):
            tw = int(ImageFont.truetype(self.font(self.top["font_label"]), round(9 * 1.15 * 1.6)).getlength(label) * 1.2)
        else:
            tw = int(len(label) * 10 * 1.15 - 1.15)
        return (tw + 36 + 28, 52) if self.td3 else (tw + 36, 39)


# --- drawing ------------------------------------------------------------------------------------
def knob_strip(r, look, pal, under, frames):
    """frames x (2r+10)^2 stacked vertically; frame k shows the value k/(frames-1). No arc closer than half a
    frame to its start: the two frames either side of a bipolar knob's centre (an even count) read as centred."""
    Image, ImageDraw, _ = _pil()
    s = 2 * r + 10
    strip = Image.new("RGB", (s, s * frames), rgb(under))
    for k in range(frames):
        strip.paste(knob_frame(r, k / (frames - 1), look, pal, under, 135.0 / (frames - 1) + 1e-6), (0, k * s))
    return strip


def knob_frame(r, t, look, pal, under, min_arc=0.5):
    Image, ImageDraw, _ = _pil()
    s = 2 * r + 10
    im = Image.new("RGB", (s * SS, s * SS), rgb(under))
    d = ImageDraw.Draw(im)
    c = (s * SS - 1) / 2.0   # Pillow's coordinates are pixel centres

    def pt(rad, deg):   # degrees clockwise from 12 o'clock
        a = math.radians(deg)
        return (c + rad * math.sin(a), c - rad * math.cos(a))

    def disc(x, y, rad, col):
        d.ellipse((x - rad, y - rad, x + rad, y + rad), fill=col)

    width = look["track"] * SS
    track_r = (r + 1) * SS

    def arc(a0, a1, col):   # a round-capped stroke along the track circle
        if abs(a1 - a0) < min_arc:
            return
        n = max(2, int(math.ceil(abs(a1 - a0))))
        outer = [pt(track_r + width / 2.0, a0 + (a1 - a0) * i / n) for i in range(n + 1)]
        inner = [pt(track_r - width / 2.0, a1 - (a1 - a0) * i / n) for i in range(n + 1)]
        d.polygon(outer + inner, fill=col)
        for a in (a0, a1):
            disc(*pt(track_r, a), width / 2.0, col)

    ang = -135.0 + 270.0 * t
    start = 0.0 if look["bipolar"] else -135.0
    arc(-135.0, 135.0, rgb(pal["line"]))
    arc(min(start, ang), max(start, ang), rgb(pal["accent"]))
    face = (r - 6) * SS
    disc(c, c, face, rgb(pal["knob_ring"]))
    disc(c, c, face - 2 * SS, rgb(pal["knob_face"]))
    pw = look["pointer"] * SS
    (x0, y0), (x1, y1) = pt(face * 0.28, ang), pt(face * 0.86, ang)
    nx, ny = (y1 - y0), -(x1 - x0)   # the pointer as a quad with round ends
    ln = math.hypot(nx, ny) or 1.0
    nx, ny = nx / ln * pw / 2.0, ny / ln * pw / 2.0
    ink = rgb(pal["ink"])
    d.polygon([(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)], fill=ink)
    disc(x0, y0, pw / 2.0, ink)
    disc(x1, y1, pw / 2.0, ink)
    return im.resize((s, s), _resample(Image, "BOX"))


def meter_name(w, h):   # shadow_skin: "sh_meter_%dx%d%s" % (w, h, sfx), no look = no suffix
    return "sh_meter_%dx%d.png" % (w, h)


def meter_strip(w, h, pal, under, frames):
    """frames x max(w,h)^2 stacked vertically (shadow_skin square_strip()), transparent around the w x h
    column. The column is the display colour (a row of them, side by side, is one screen). Frame k: the
    value k/(frames-1) as a bar from the middle (2 px clear each side, so the bars stand apart), the zero
    line over the full width (one axis across the row)."""
    Image, ImageDraw, _ = _pil()
    sq = max(w, h)
    im = Image.new("RGBA", (sq, sq * frames), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, y0 = (sq - w) // 2, (sq - h) // 2
    mid, half = y0 + h // 2, h // 2 - 2
    card, bar, axis = rgb(pal["lcd"]) + (255,), rgb(pal["accent"]) + (255,), rgb(pal["line"]) + (255,)
    for k in range(frames):
        oy = k * sq
        d.rectangle([x0, oy + y0, x0 + w - 1, oy + y0 + h - 1], fill=card)
        v = 2.0 * k / (frames - 1) - 1.0
        n = int(round(abs(v) * half))
        if n:
            top, bot = (mid - n, mid - 1) if v > 0 else (mid + 1, mid + n)
            d.rectangle([x0 + 2, oy + top, x0 + w - 3, oy + bot], fill=bar)
        d.line([x0, oy + mid, x0 + w - 1, oy + mid], fill=axis)
    return im


def button_image(w, h, label, primary, on, pal, under, font_path):
    """A rounded button 2 px inside the image (48 px tall in a td3 skin's 52), a 2 px ring, the label centred."""
    Image, ImageDraw, ImageFont = _pil()
    fill = (pal["accent_hi"] if on else pal["accent"]) if primary else shade(pal["btn_bg"], 1.35 if on else 1.0)
    ring = fill if primary else pal["line"]
    ink = pal["seg_active_tx"] if primary else pal["ink"]
    im = Image.new("RGB", (w * SS, h * SS), rgb(under))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((2 * SS, 2 * SS, (w - 2) * SS - 1, (h - 2) * SS - 1), radius=10 * SS, fill=rgb(ring))
    d.rounded_rectangle((4 * SS, 4 * SS, (w - 4) * SS - 1, (h - 4) * SS - 1), radius=8 * SS, fill=rgb(fill))
    im = im.resize((w, h), _resample(Image, "BOX"))
    d = ImageDraw.Draw(im)
    font = _font(font_path, BUTTON_TEXT_PX)
    tb = d.textbbox((0, 0), label, font=font)
    d.text(((w - (tb[2] - tb[0])) / 2.0 - tb[0], (h - (tb[3] - tb[1])) / 2.0 - tb[1]), label, font=font, fill=rgb(ink))
    return im


def arrow_image(h, side, pal, under):
    """A stepper arrow as the design draws it: an h x h box rounded 5 (render_conf_preview.c widget_stepper()) in
    the line colour, a centred triangle in accent_hi (11 x 16 px at h=40), 4x supersampled."""
    Image, ImageDraw, _ = _pil()
    im = Image.new("RGB", (h * SS, h * SS), rgb(under))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, h * SS - 1, h * SS - 1), radius=ARROW_RADIUS * SS, fill=rgb(pal["line"]))
    c = (h * SS - 1) / 2.0
    hw, hh = h * SS * 11 / 80.0, h * SS * 16 / 80.0   # half width, half height
    tip = -1 if side == "prev" else 1
    d.polygon([(c + tip * hw, c), (c - tip * hw, c - hh), (c - tip * hw, c + hh)], fill=rgb(pal["accent_hi"]))
    return im.resize((h, h), _resample(Image, "BOX"))


# --- the polish ---------------------------------------------------------------------------------
def plan(skin_dir, layout_path, style):
    """Check the skin folder against the layout: ([(file name, function drawing its new image)], Skin).
    Raises PolishError listing every problem."""
    Image, _, _ = _pil()
    if not os.path.isdir(skin_dir):
        raise PolishError("%s: no such skin folder (run gen_vst.py first)" % skin_dir)
    tui_path = os.path.join(skin_dir, "TUI.json")
    if not os.path.isfile(tui_path):
        raise PolishError("%s: no TUI.json: not a generated skin folder" % skin_dir)
    tui = open(tui_path, encoding="utf-8").read()
    sk = Skin(layout_path, style)
    pal, frames = style["palette"], int(style["frames"])
    errors, out = [], []

    def image(name, size):
        """The generator's image `name`, checked: it exists, TUI.json uses it, it has the generator's size."""
        path = os.path.join(skin_dir, name)
        if not os.path.isfile(path):
            errors.append("%s: missing" % name)
            return None
        if '"%s"' % name not in tui:
            errors.append("%s: TUI.json does not use it (wrong layout for this skin?)" % name)
        try:
            with Image.open(path) as im:
                got = im.size
        except OSError as e:
            errors.append("%s: not an image (%s)" % (name, e))
            return None
        if size and got != size:
            errors.append("%s: is %dx%d, expected %dx%d" % ((name,) + got + size))
            return None
        return got

    on_disk = set(os.listdir(skin_dir))
    for prefix, known in (("sh_knob_r", {"sh_knob_r%d.png" % r for r in sk.knobs}),
                          ("sh_btn_", {"%s_%s.png" % (b, s) for b in sk.buttons for s in ("on", "off")}),
                          ("sh_arrow_", set(sk.arrows)),
                          ("sh_meter_", {meter_name(w, h) for w, h in sk.meters})):
        for f in sorted(on_disk):
            if f.startswith(prefix) and f.endswith(".png") and f not in known:
                errors.append("%s: not in the layout (wrong layout for this skin?)" % f)

    for r in sk.knobs:
        look = style["knobs"].get(str(r))
        if not look:
            errors.append("knob radius %d: no look in the style (skin_style.json knobs)" % r)
            continue
        s = 2 * r + 10
        if image("sh_knob_r%d.png" % r, (s, s * frames)):
            out.append(("sh_knob_r%d.png" % r, lambda r=r, look=look: knob_strip(r, look, pal, sk.under, frames)))

    if sk.buttons:
        font = sk.font(style["title_font"])
    for stem, (key, label) in sorted(sk.buttons.items()):
        size = sk.button_size(label)
        primary = key in style["primary_buttons"]
        for state in ("on", "off"):
            if image("%s_%s.png" % (stem, state), size):
                out.append(("%s_%s.png" % (stem, state), lambda size=size, label=label, primary=primary, on=state == "on":
                            button_image(size[0], size[1], label, primary, on, pal, sk.under, font)))

    for name, (side, h) in sorted(sk.arrows.items()):
        if image(name, (h, h)):
            out.append((name, lambda h=h, side=side: arrow_image(h, side, pal, sk.under)))

    for w, h in sk.meters:
        sq = max(w, h)
        if image(meter_name(w, h), (sq, sq * frames)):
            out.append((meter_name(w, h), lambda w=w, h=h: meter_strip(w, h, pal, sk.under, frames)))

    pages = regroup(skin_dir, tui, style["tab_groups"], errors)
    if errors:
        raise PolishError("skin polish refused, nothing written:\n  " + "\n  ".join(errors))
    return out, sk, pages


def regroup(skin_dir, tui_text, groups, errors):
    """5. The generator numbers its tabs fnKeyIndex = place, fnKeySubIndex = 0 (Q-Links files: Tab = place + 1,
    SubTab = 1). Renumbered: fnKeyIndex = the group, fnKeySubIndex = the page in it. A skin numbered so already is
    left as it is; any other numbering, or tabs that are not the groups' pages in order, is refused. Returns
    [(file name, JSON object)] to write."""
    names = [n for g in groups for n in g["pages"]]
    want = [(gi, si) for gi, g in enumerate(groups) for si in range(len(g["pages"]))]
    try:
        tui = json.loads(tui_text)
        tabs = tui["pageData"]["tabs"]
    except (ValueError, KeyError, TypeError):
        errors.append("TUI.json: no pageData tabs (not a generated skin folder?)")
        return []
    got = [t.get("tabName") for t in tabs]
    if not names or got != names:
        errors.append("TUI.json has the tabs %s, the page groups list %s (wrong layout for this skin?)" % (got, names))
        return []
    plain = [(i, 0) for i in range(len(tabs))]
    now = [(t.get("fnKeyIndex"), t.get("fnKeySubIndex")) for t in tabs]
    if now not in (plain, want):
        errors.append("TUI.json tabs are numbered %s: neither the generator's numbering nor the groups'" % now)
        return []
    for t, (gi, si) in zip(tabs, want):
        t["fnKeyIndex"], t["fnKeySubIndex"] = gi, si
    out = [("TUI.json", tui)]
    for f in ("Q-Links.json", "Q-Links - 8by1.json"):   # the generator writes both, the same
        path = os.path.join(skin_dir, f)
        if not os.path.isfile(path):
            if f == "Q-Links.json":
                errors.append("%s: missing" % f)
            continue
        try:
            q = json.load(open(path, encoding="utf-8"))
            sets = q["Screen Mode Q-Links"]["map"]
            now = [(e["Tab"] - 1, e["SubTab"] - 1) for e in sets]
        except (OSError, ValueError, KeyError, TypeError):
            errors.append("%s: no Screen Mode Q-Links sets" % f)
            continue
        if now not in (plain, want):
            errors.append("%s: Q-Link sets numbered %s do not match the %d pages" % (f, now, len(tabs)))
            continue
        for e, (gi, si) in zip(sets, want):
            e["Tab"], e["SubTab"] = gi + 1, si + 1
        out.append((f, q))
    return out


def polish(skin_dir, layout_path, style_path):
    if not os.path.isfile(layout_path):
        raise PolishError("%s: no such layout" % layout_path)
    try:
        style = json.load(open(style_path, encoding="utf-8"))
        for k in ("palette", "title_font", "frames", "knobs", "primary_buttons", "tab_groups"):
            style[k]
    except (OSError, ValueError, KeyError) as e:
        raise PolishError("%s: not a skin style from surface.py (%s)" % (style_path, e))
    todo, sk, pages = plan(skin_dir, layout_path, style)
    drawn = [(name, make()) for name, make in todo]   # draw everything before writing anything
    texts = [(name, json.dumps(obj, indent=4)) for name, obj in pages]
    for name, im in drawn:
        path = os.path.join(skin_dir, name)
        im.save(path + ".tmp.png")
        os.replace(path + ".tmp.png", path)
    for name, text in texts:
        path = os.path.join(skin_dir, name)
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(path + ".tmp", path)
    return "skin polish: %d knob strips, %d button images, %d stepper arrows, %d meter strips, %d pages in %d tabs" % (
        len(sk.knobs), 2 * len(sk.buttons), len(sk.arrows), len(sk.meters), sum(len(g["pages"]) for g in style["tab_groups"]),
        len(style["tab_groups"]))


# --- self-test ----------------------------------------------------------------------------------
def selftest(samples=None):
    """A fabricated skin folder for the real layout (names and sizes as shadow_skin writes them, a dummy
    colour), polished and checked; then the refusals."""
    Image, ImageDraw, _ = _pil()
    sys.path.insert(0, HERE)
    import surface
    layout_text, style = surface.pages(), surface.skin_style()
    surface.check_layout(layout_text, style["tab_groups"])
    tmp = tempfile.mkdtemp(prefix="pf_polish_")
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)

    try:
        lay = os.path.join(tmp, "layout.conf")
        open(lay, "w").write(layout_text)
        os.symlink(os.path.join(HERE, "fonts"), os.path.join(tmp, "fonts"))   # font paths are layout-relative
        sty = os.path.join(tmp, "skin_style.json")
        json.dump(style, open(sty, "w"))
        sk = Skin(lay, style)
        pal = style["palette"]
        dummy = (255, 0, 255)

        def fabricate(folder):
            os.makedirs(folder)
            names = []
            for r in sk.knobs:
                s = 2 * r + 10
                names.append(("sh_knob_r%d.png" % r, (s, s * style["frames"])))
            for stem, (_, label) in sk.buttons.items():
                names += [("%s_%s.png" % (stem, st), sk.button_size(label)) for st in ("on", "off")]
            names += [(n, (h, h)) for n, (_, h) in sk.arrows.items()]
            names += [(meter_name(w, h), (max(w, h), max(w, h) * style["frames"])) for w, h in sk.meters]
            names += [("sh_bg_0.png", (1280, 628)), ("sh_seg_engine_0_on.png", (102, 33))]   # left alone
            for n, size in names:
                Image.new("RGB", size, dummy).save(os.path.join(folder, n))
            # the tabs and Q-Link sets as the generator numbers them: one strip button per [tab]
            tabs = [{"tabName": t["name"], "fnKeyIndex": i, "fnKeySubIndex": 0} for i, t in enumerate(sk.tabs)]
            json.dump({"images": [n for n, _ in names], "pageData": {"tabs": tabs}},
                      open(os.path.join(folder, "TUI.json"), "w"))
            sets = {"Screen Mode Q-Links": {"map": [{"Tab": i + 1, "SubTab": 1} for i in range(len(tabs))]}}
            for f in ("Q-Links.json", "Q-Links - 8by1.json"):
                json.dump(sets, open(os.path.join(folder, f), "w"))
            return dict(names)

        def numbering(folder):
            t = json.load(open(os.path.join(folder, "TUI.json")))["pageData"]["tabs"]
            q = [json.load(open(os.path.join(folder, f)))["Screen Mode Q-Links"]["map"]
                 for f in ("Q-Links.json", "Q-Links - 8by1.json")]
            return ([(x["fnKeyIndex"], x["fnKeySubIndex"]) for x in t],
                    [[(e["Tab"] - 1, e["SubTab"] - 1) for e in m] for m in q])

        skin = os.path.join(tmp, "ok", "Plugin Skins")
        sizes = fabricate(skin)
        print(polish(skin, lay, sty))
        grouped = [(gi, si) for gi, g in enumerate(style["tab_groups"]) for si in range(len(g["pages"]))]
        check(numbering(skin) == (grouped, [grouped, grouped]), "tabs and Q-Link sets are not numbered by page group")
        check(any(si for _, si in grouped) and len(style["tab_groups"]) <= 7, "the groups have no sub-pages, or too many")
        polish(skin, lay, sty)   # a skin grouped already stays as it is
        check(numbering(skin) == (grouped, [grouped, grouped]), "polishing twice changed the grouping")
        for n, size in sizes.items():
            with Image.open(os.path.join(skin, n)) as im:
                check(im.size == size, "%s changed size" % n)
                pix = set(pixels(im))
            touched = not n.startswith(("sh_bg_", "sh_seg_"))
            check((dummy not in pix) == touched, "%s %s" % (n, "still has the dummy colour" if touched else "was touched"))

        under, line, accent = rgb(sk.under), rgb(pal["line"]), rgb(pal["accent"])

        def near(a, b, tol=12):
            return all(abs(x - y) <= tol for x, y in zip(a, b))

        for r in sk.knobs:
            look, s = style["knobs"][str(r)], 2 * r + 10
            strip = Image.open(os.path.join(skin, "sh_knob_r%d.png" % r)).convert("RGB")
            fr = lambda k: strip.crop((0, k * s, s, (k + 1) * s))
            last, mid, first = fr(style["frames"] - 1), fr((style["frames"] - 1) // 2), fr(0)
            c = s // 2
            top = (c, s // 2 - (r + 1))   # 12 o'clock on the track
            right = (s // 2 + int(round((r + 1) * math.sin(math.radians(90)))), c)   # 3 o'clock
            check(first.getpixel((0, 0)) == under, "r%d: corner is not the card colour" % r)
            check(near(first.getpixel(right), line), "r%d: value 0: track at 3 o'clock is not the line colour" % r)
            check(near(last.getpixel(right), accent), "r%d: value 1: arc at 3 o'clock is not the accent" % r)
            if look["bipolar"]:   # the arc grows from 12 o'clock: none at the centre value, accent at both ends
                check(not any(near(p, accent, 30) for p in pixels(mid)), "r%d bipolar: the centre frame has an arc" % r)
                check(near(first.getpixel((s - 1 - right[0], c)), accent), "r%d bipolar: value 0: no arc at 9 o'clock" % r)
            else:
                check(not any(near(p, accent, 30) for p in pixels(first)), "r%d: value 0 has an arc" % r)
                check(near(mid.getpixel(top), accent) or near(mid.getpixel((top[0] - 1, top[1])), accent),
                      "r%d: value 0.5: no arc at 12 o'clock" % r)
            row = [last.getpixel((x, c)) for x in range(c - (r - 4), c)]   # 9 o'clock to the centre
            check(any(near(p, rgb(pal["knob_ring"]), 3) for p in row), "r%d: no face ring" % r)
            check(any(near(p, rgb(pal["knob_face"]), 3) for p in row), "r%d: no inner face" % r)
        for stem, (key, label) in sk.buttons.items():
            primary = key in style["primary_buttons"]
            for st in ("on", "off"):
                im = Image.open(os.path.join(skin, "%s_%s.png" % (stem, st))).convert("RGB")
                w, h = im.size
                check(im.getpixel((0, 0)) == under, "%s_%s: corner is not the card colour" % (stem, st))
                fill = rgb((pal["accent_hi"] if st == "on" else pal["accent"]) if primary
                           else shade(pal["btn_bg"], 1.35 if st == "on" else 1.0))
                check(near(im.getpixel((6, h // 2)), fill, 2), "%s_%s: body is not the fill colour" % (stem, st))
                ink = rgb(pal["seg_active_tx"] if primary else pal["ink"])
                check(any(near(p, ink, 40) for p in pixels(im.crop((w // 4, h // 4, 3 * w // 4, 3 * h // 4)))),
                      "%s_%s: no label" % (stem, st))
        for n, (side, h) in sk.arrows.items():
            im = Image.open(os.path.join(skin, n)).convert("RGB")
            glyph = rgb(pal["accent_hi"])
            check(im.getpixel((h // 2, h // 2)) == glyph, "%s: no arrow in the middle" % n)
            check(im.getpixel((0, 0)) != line and im.getpixel((h // 2, 1)) == line, "%s: box" % n)
            back, front = (h // 2 + 4, h // 2 - 5), (h // 2 - 5, h // 2 - 5)   # the flat side is tall, the tip narrow
            if side == "next":
                back, front = (h - 1 - back[0], back[1]), (h - 1 - front[0], front[1])
            check(im.getpixel(back) == glyph and im.getpixel(front) == line, "%s: points the wrong way" % n)

        check(len(sk.meters) >= 1, "the layout has no wave view meters")
        for mw, mh in sk.meters:
            sq, fr_n = max(mw, mh), style["frames"]
            strip = Image.open(os.path.join(skin, meter_name(mw, mh))).convert("RGBA")
            x0, y0 = (sq - mw) // 2, (sq - mh) // 2
            cx, mid = x0 + mw // 2, y0 + mh // 2

            def px(k, x, y):
                return strip.getpixel((x, k * sq + y))
            check(sq == mw or px(0, 0, 0)[3] == 0, "meter %dx%d: padding is not transparent" % (mw, mh))
            check(near(px(fr_n - 1, cx, mid - mh // 4)[:3], accent), "meter %dx%d: value 1: no bar above the line" % (mw, mh))
            check(near(px(0, cx, mid + mh // 4)[:3], accent), "meter %dx%d: value 0: no bar below the line" % (mw, mh))
            check(not near(px(fr_n - 1, cx, mid + mh // 4)[:3], accent), "meter %dx%d: value 1 has a bar below" % (mw, mh))
            check(not any(near(px(fr_n // 2, cx, y)[:3], accent) for y in range(y0, mid - 2)),
                  "meter %dx%d: the middle value has a bar above the line" % (mw, mh))
            check(near(px(fr_n // 2, x0, mid)[:3], line) and near(px(fr_n // 2, x0 + mw - 1, mid)[:3], line),
                  "meter %dx%d: the zero line does not run the full width" % (mw, mh))
            check(px(fr_n - 1, x0, mid - mh // 4)[:3] == rgb(pal["lcd"]), "meter %dx%d: no display colour beside the bar" % (mw, mh))

        # refusals: each leaves every file untouched
        def refused(desc, mutate, needle):
            folder = os.path.join(tmp, desc.replace(" ", "_"), "Plugin Skins")
            fabricate(folder)
            mutate(folder)
            try:
                polish(folder, lay, sty)
                fails.append("%s: polished anyway" % desc)
                return
            except PolishError as e:
                check(needle in str(e), "%s: message %r" % (desc, str(e)[:200]))
            for f in os.listdir(folder):
                if f.endswith(".png"):
                    with Image.open(os.path.join(folder, f)) as im:
                        check(im.getpixel((0, 0)) == dummy, "%s: %s was written although refused" % (desc, f))
        some_btn = sorted(sk.buttons)[0] + "_on.png"
        some_arrow = sorted(sk.arrows)[0]
        refused("missing button", lambda f: os.remove(os.path.join(f, some_btn)), some_btn + ": missing")
        refused("wrong knob size", lambda f: Image.new("RGB", (70, 70 * 127), dummy).save(os.path.join(f, "sh_knob_r30.png")),
                "sh_knob_r30.png: is 70x8890")
        refused("wrong arrow size", lambda f: Image.new("RGB", (40, 41), dummy).save(os.path.join(f, some_arrow)),
                some_arrow + ": is 40x41")
        refused("unknown button", lambda f: Image.new("RGB", (60, 52), dummy).save(os.path.join(f, "sh_btn_zz_ZZ_on.png")),
                "sh_btn_zz_ZZ_on.png: not in the layout")
        refused("not in TUI", lambda f: json.dump({"images": []}, open(os.path.join(f, "TUI.json"), "w")),
                "TUI.json does not use it")
        refused("no TUI", lambda f: os.remove(os.path.join(f, "TUI.json")), "no TUI.json")
        refused("unknown meter", lambda f: Image.new("RGB", (9, 9 * 128), dummy).save(os.path.join(f, "sh_meter_9x9.png")),
                "sh_meter_9x9.png: not in the layout")

        def edit_tabs(f, change):
            path = os.path.join(f, "TUI.json")
            t = json.load(open(path))
            change(t["pageData"]["tabs"])
            json.dump(t, open(path, "w"))
        refused("pages out of order", lambda f: edit_tabs(f, lambda ts: ts.insert(0, ts.pop(1))), "the page groups list")
        refused("odd numbering", lambda f: edit_tabs(f, lambda ts: ts[1].update(fnKeySubIndex=3)),
                "neither the generator's numbering nor the groups'")
        try:
            polish(os.path.join(tmp, "nowhere"), lay, sty)
            fails.append("a missing skin folder was accepted")
        except PolishError:
            pass

        if samples:
            write_samples(samples, sk, style, skin)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if fails:
        raise PolishError("selftest FAILED:\n  " + "\n  ".join(fails))
    return "skin polish selftest: ok (%d knob radii, %d buttons, %d arrows, %d meter strips, %d pages, 9 refusals)" % (
        len(sk.knobs), len(sk.buttons), len(sk.arrows), len(sk.meters), len(sk.tabs))


def write_samples(out, sk, style, skin):
    """Sample renders for a human look: a frame grid per knob radius, the buttons, the arrows (2x zoom)."""
    Image, ImageDraw, ImageFont = _pil()
    os.makedirs(out, exist_ok=True)
    under = rgb(sk.under)
    for r in sk.knobs:
        s = 2 * r + 10
        strip = Image.open(os.path.join(skin, "sh_knob_r%d.png" % r))
        picks = [0, 16, 32, 48, 63, 64, 80, 96, 112, 127]
        grid = Image.new("RGB", (len(picks) * (s + 8) + 8, s + 16), under)
        for i, k in enumerate(picks):
            grid.paste(strip.crop((0, k * s, s, (k + 1) * s)), (8 + i * (s + 8), 8))
        grid.resize((grid.width * 2, grid.height * 2), _resample(Image, "NEAREST")).save(os.path.join(out, "knob_r%d_frames.png" % r))
    imgs = []
    for stem in sorted(sk.buttons):
        for st in ("off", "on"):
            imgs.append(Image.open(os.path.join(skin, "%s_%s.png" % (stem, st))))
    for name in sorted(sk.arrows):
        imgs.append(Image.open(os.path.join(skin, name)))
    x, y, rh = 8, 8, 0
    sheet = Image.new("RGB", (900, 600), under)
    for im in imgs:
        if x + im.width + 8 > sheet.width:
            x, y, rh = 8, y + rh + 8, 0
        sheet.paste(im, (x, y))
        x, rh = x + im.width + 8, max(rh, im.height)
    sheet = sheet.crop((0, 0, sheet.width, y + rh + 8))
    sheet.resize((sheet.width * 2, sheet.height * 2), _resample(Image, "NEAREST")).save(os.path.join(out, "buttons_arrows.png"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("skin", nargs="?", help='the skin\'s "Plugin Skins" folder')
    ap.add_argument("--layout", default=os.path.join(HERE, "layout.conf"))
    ap.add_argument("--style", default=os.path.join(HERE, "build", "skin_style.json"))
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--samples", help="with --selftest: write sample renders here")
    a = ap.parse_args()
    try:
        if a.selftest:
            print(selftest(a.samples))
        elif a.skin:
            print(polish(a.skin, a.layout, a.style))
        else:
            ap.error("give the skin folder, or --selftest")
    except PolishError as e:
        sys.exit("skin_polish: %s" % e)


if __name__ == "__main__":
    main()
