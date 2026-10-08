#!/usr/bin/env python3
"""gen_themes.py -- turn the design mockups into src/ftheme_data.h.

Usage: python tools/gen_themes.py [design-folder] > src/ftheme_data.h
       (default folder: E:/filemanager-top-20-design)

Each designs/NN-name.html holds its tokens in `.mode-dark .m{...}` and
`.mode-light .m{...}` rules; shared defaults sit in the first `--r:` block.
The output is plain C data, so the build never needs Python.
"""
import glob
import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "E:/filemanager-top-20-design"
TYPES = ["folder", "image", "audio", "video", "archive", "code", "pdf", "doc", "text"]


def names_from_readme():
    out = {}
    txt = open(os.path.join(ROOT, "README.md"), encoding="utf-8").read()
    for num, name, pitch in re.findall(r"^\| (\d\d) \| \[([^\]]+)\]\([^)]*\) \| [^|]+\| (.+?) \|$", txt, re.M):
        out[int(num)] = (name, pitch)
    return out


def parse_color(v, env):
    v = v.strip()
    m = re.fullmatch(r"var\((--[\w-]+)\)", v)
    if m:
        return parse_color(env[m.group(1)], env)
    if v == "transparent":
        return 0x00000000
    m = re.fullmatch(r"#([0-9a-fA-F]{6})", v)
    if m:
        return 0xFF000000 | int(m.group(1), 16)
    m = re.fullmatch(r"#([0-9a-fA-F]{3})", v)
    if m:
        return 0xFF000000 | int("".join(c * 2 for c in m.group(1)), 16)
    m = re.fullmatch(r"rgba?\(([^)]*)\)", v)
    if m:
        p = [x.strip() for x in m.group(1).split(",")]
        a = float(p[3]) if len(p) > 3 else 1.0
        r, g, b = (int(float(x)) for x in p[:3])
        return (round(a * 255) << 24) | (r << 16) | (g << 8) | b
    raise ValueError("colour? " + v)


def split_top(s):
    parts, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    return [p.strip() for p in parts if p.strip()]


def parse_shadow(v, env):
    """-> (kind, dx, dy, colour); kind 0 soft, 1 none, 2 hard offset."""
    v = v.strip()
    if v == "none":
        return 1, 0, 0, 0
    layer = split_top(v)[-1]           # the largest layer reads best
    m = re.match(r"(-?[\d.]+)(?:px)?\s+(-?[\d.]+)(?:px)?\s+([\d.]+)(?:px)?\s+(.*)$", layer)
    dx, dy, blur, col = float(m.group(1)), float(m.group(2)), float(m.group(3)), m.group(4)
    c = parse_color(col, env)
    if blur == 0:
        return 2, int(dx), int(dy), c
    a = min(255, round(((c >> 24) & 255) * 1.6))   # gfx_shadow fades faster than CSS
    return 0, 0, 0, (a << 24) | (c & 0xFFFFFF)


def px(v):
    return float(v.strip().replace("px", ""))


def main():
    names = names_from_readme()
    variants, themes = [], []
    for path in sorted(glob.glob(os.path.join(ROOT, "designs", "*.html"))):
        num = int(os.path.basename(path)[:2])
        lines = open(path, encoding="utf-8").read().split("\n")
        base = {}
        for l in lines:
            if l.startswith("--r:") or ":root" in l:
                pass
        # shared defaults: the first rule that sets --rowh without a mode selector
        txt = "\n".join(lines)
        m = re.search(r"\{([^{}]*--rowh:[^{}]*--c-folder:[^{}]*)\}", txt)
        for k, v in re.findall(r"(--[\w-]+):([^;}]+)", m.group(1)):
            base[k] = v.strip()
        gradient = None
        g = re.search(r"^\.m\{background:linear-gradient\(180deg,([^)]*)\)\}", txt, re.M)
        if g:
            stops = re.findall(r"(#[0-9A-Fa-f]{6})\s+(\d+)%", g.group(1))
            gradient = [(parse_color(c, {}), int(p)) for c, p in stops]
        idx = {"dark": -1, "light": -1}
        for l in lines:
            mm = re.match(r"\.mode-(dark|light) \.m\{(.*)\}\s*$", l)
            if not mm:
                continue
            env = dict(base)
            for k, v in re.findall(r"(--[\w-]+):([^;}]+)", mm.group(2)):
                env[k] = v.strip()
            dark = mm.group(1) == "dark"
            kind, sdx, sdy, shc = parse_shadow(env["--shadow"], env)
            st = env.get("--sel-text", "var(--text)")
            v = dict(
                dark=dark,
                bg=parse_color(env["--bg"], env),
                panel=parse_color(env["--panel"], env),
                raised=parse_color(env["--raised"], env),
                line=parse_color(env["--line"], env),
                text=parse_color(env["--text"], env),
                text2=parse_color(env["--text2"], env),
                accent=parse_color(env["--accent"], env),
                on_accent=parse_color(env["--on-accent"], env),
                sel=parse_color(env["--sel"], env),
                sel_text=0 if st == "var(--text)" else parse_color(st, env),
                border=parse_color(env.get("--panel-border", "transparent"), env),
                danger=parse_color(env.get("--danger", "#EF5350"), env),
                shadow=shc, shadow_kind=kind, sdx=sdx, sdy=sdy,
                r=px(env["--r"]), rs=px(env["--rs"]), rowh=px(env["--rowh"]), fs=px(env["--fs"]),
                types=[parse_color(env["--c-" + t], env) for t in TYPES],
                grad=gradient,
            )
            idx[mm.group(1)] = len(variants)
            variants.append(v)
        name, pitch = names.get(num, (os.path.basename(path)[3:-5], ""))
        themes.append((name, pitch, idx["dark"], idx["light"]))

    h = lambda c: "0x%08X" % c
    fl = lambda x: ("%.1ff" % x) if x != int(x) else ("%d.0f" % x)
    o = []
    o.append("/* ftheme_data.h -- GENERATED by tools/gen_themes.py from the design mockups.")
    o.append("** Do not edit by hand; regenerate instead. Colours are 0xAARRGGBB. */")
    o.append("")
    o.append("static const FmThemeVariant kVariants[] = {")
    for v in variants:
        gr = v["grad"] or []
        g0 = h(gr[0][0]) if gr else "0"
        g1 = h(gr[len(gr) // 2][0]) if len(gr) > 2 else (g0 if gr else "0")
        g2 = h(gr[-1][0]) if gr else "0"
        gm = gr[len(gr) // 2][1] if len(gr) > 2 else 50
        o.append("  { %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s," % (
            "true" if v["dark"] else "false", h(v["bg"]), h(v["panel"]), h(v["raised"]), h(v["line"]),
            h(v["text"]), h(v["text2"]), h(v["accent"]), h(v["on_accent"]), h(v["sel"]),
            h(v["sel_text"]), h(v["border"]), h(v["danger"]), h(v["shadow"])))
        o.append("    %d, %d, %d, %s, %s, %s, %s, { %s, %s, %s, %d }," % (
            v["shadow_kind"], v["sdx"], v["sdy"], fl(v["r"]), fl(v["rs"]), fl(v["rowh"]), fl(v["fs"]), g0, g1, g2, gm))
        o.append("    { %s } }," % ", ".join(h(c) for c in v["types"]))
    o.append("};")
    o.append("")
    o.append("static const FmThemeInfo kThemes[THEME_COUNT] = {")
    for name, pitch, d, l in themes:
        pitch = pitch.replace("\\", "\\\\").replace('"', '\\"')
        o.append('  { "%s", "%s", %d, %d },' % (name, pitch, d, l))
    o.append("};")
    print("\n".join(o))


if __name__ == "__main__":
    main()
