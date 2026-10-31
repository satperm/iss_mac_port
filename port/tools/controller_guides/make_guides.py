#!/usr/bin/env python3
"""Makes the Discord guides for the ISS PC motion-sensor / touchpad controls: one image per
controller (Xbox, Nintendo Switch Pro, 8BitDo) and language (pt-BR, en-US), with numbered circles on
the buttons of each instruction and the instructions written beside the controller.

Usage: make_guides.py <controller images dir> <output dir> [--debug]
Inputs in the images dir: 1.webp (Xbox), 2.png (Switch Pro), 3.png (8BitDo).
"""
import math
import sys
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw, ImageFont

FONT_VAR = "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf"
FONT_FALLBACK = "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf"
SS = 2  # supersampling

COL = {1: "#ff6a2b", 2: "#ff3d9a", 3: "#19c8e8", 4: "#4d8bff", 5: "#f5c400", 6: "#2fc47a", 7: "#b070ff"}


def font(size, bold=True):
    try:
        f = ImageFont.truetype(FONT_VAR, size * SS)
        f.set_variation_by_name("Bold" if bold else "Regular")
        return f
    except Exception:
        return ImageFont.truetype(FONT_FALLBACK, size * SS)


# Positions in the ORIGINAL image pixels: name -> (x, y, radius). (Xbox: measured on the 1932 px view
# of the 2000 px image, scaled by 1.04.)
def xbox():
    s = 1.04
    t = {
        "L3": (525, 722, 92), "R3": (1190, 985, 92), "VIEW": (840, 708, 44), "SHARE": (965, 805, 42),
        "A": (1408, 832, 52), "RB": (1385, 425, 78), "RT": (1560, 290, 40),
    }
    return {k: (x * s, y * s, r * s) for k, (x, y, r) in t.items()}


TARGETS = {
    "xbox": xbox(),
    "switch": {"L3": (172, 178, 62), "R3": (508, 290, 62), "VIEW": (308, 121, 26), "SHARE": (348, 181, 24),
               "A": (617, 238, 30), "RB": (640, 38, 46), "RT": (745, -22, 26)},
    "8bitdo": {"L3": (125, 235, 58), "R3": (312, 292, 56), "VIEW": (200, 193, 21), "SHARE": (283, 190, 24),
               "A": (381, 262, 26), "RB": (393, 163, 30), "RT": (440, 112, 26)},
}

# Step -> list of (target, ring level). The same button used in several steps gets bigger rings.
STEPS = {
    "xbox":   {1: [("L3", 0), ("R3", 0), ("SHARE", 0)], 2: [("A", 0)], 3: [("RT", 0)], 4: [("L3", 1)],
               5: [("VIEW", 0)], 6: [("VIEW", 1), ("R3", 1)], 7: [("VIEW", 2), ("RB", 0)]},
    "switch": {1: [("L3", 0), ("R3", 0), ("SHARE", 0)], 2: [("A", 0)], 3: [("RT", 0)], 4: [("L3", 1)],
               5: [("VIEW", 0)], 6: [("VIEW", 1), ("R3", 1)], 7: [("VIEW", 2), ("RB", 0)]},
    "8bitdo": {1: [("L3", 0), ("R3", 0)], 2: [("A", 0)], 3: [("RT", 0)], 4: [("L3", 1)],
               5: [("VIEW", 0)], 6: [("VIEW", 1), ("R3", 1)], 7: [("VIEW", 2), ("RB", 0)]},
}

NAMES = {
    "xbox": ("Xbox / Xbox Series", "Xbox / Xbox Series"),
    "switch": ("Nintendo Switch Pro", "Nintendo Switch Pro"),
    "8bitdo": ("8BitDo", "8BitDo"),
}

TXT = {
    "pt": {
        "kicker": "ISS · PC",
        "title": "Modo sensor e touchpad",
        "sub": "Grafite, absorver poderes, portas e scanner",
        "foot": "Seu controle não tem giroscópio nem touchpad: o port simula os dois.",
        "steps": {
            1: ("Ligar/desligar o modo sensor", {
                "xbox": "Clique L3 + R3 (os dois analógicos) ou o botão Compartilhar (Xbox Series)",
                "switch": "Clique L3 + R3 (os dois analógicos) ou o botão Captura",
                "8bitdo": "Clique L3 + R3 (os dois analógicos)"}),
            2: ("Sacudir a lata de spray", {
                "xbox": "Botão A (o de baixo)", "switch": "Botão de baixo (B no Switch)",
                "8bitdo": "Botão A (o de baixo)"}),
            3: ("Pintar o grafite", {
                "xbox": "Gatilho direito (RT)", "switch": "Gatilho direito (ZR)", "8bitdo": "Gatilho direito (R2)"}),
            4: ("Mirar a lata", {"xbox": "Analógico esquerdo", "switch": "Analógico esquerdo",
                                 "8bitdo": "Analógico esquerdo"}),
            5: ("Clicar o touchpad", {
                "xbox": "Botão View (os dois quadrados). Segurar = segurar o touchpad (absorver poder)",
                "switch": "Botão − (menos). Segurar = segurar o touchpad (absorver poder)",
                "8bitdo": "Botão Select (à esquerda do centro). Segurar = segurar o touchpad"}),
            6: ("Deslizar no touchpad", {
                "xbox": "Segure o botão View e mova o analógico direito na direção pedida (porta 'Open Door')",
                "switch": "Segure o botão − e mova o analógico direito na direção pedida (porta 'Open Door')",
                "8bitdo": "Segure o botão Select e mova o analógico direito na direção pedida (porta 'Open Door')"}),
            7: ("Duas pontas do touchpad", {
                "xbox": "Segure o botão View e aperte RB (levantar o carro de cima do his brother)",
                "switch": "Segure o botão − e aperte R (levantar o carro de cima do his brother)",
                "8bitdo": "Segure o botão Select e aperte R1 (levantar o carro de cima do his brother)"}),
        },
    },
    "en": {
        "kicker": "ISS · PC",
        "title": "Motion sensor & touchpad",
        "sub": "Graffiti, absorbing powers, doors and the scanner",
        "foot": "Your controller has no gyro or touchpad: the port simulates both.",
        "steps": {
            1: ("Turn sensor mode on/off", {
                "xbox": "Click L3 + R3 (both sticks) or the Share button (Xbox Series)",
                "switch": "Click L3 + R3 (both sticks) or the Capture button",
                "8bitdo": "Click L3 + R3 (both sticks)"}),
            2: ("Shake the spray can", {
                "xbox": "A button (the bottom one)", "switch": "Bottom button (B on the Switch)",
                "8bitdo": "A button (the bottom one)"}),
            3: ("Spray the graffiti", {
                "xbox": "Right trigger (RT)", "switch": "Right trigger (ZR)", "8bitdo": "Right trigger (R2)"}),
            4: ("Aim the can", {"xbox": "Left stick", "switch": "Left stick", "8bitdo": "Left stick"}),
            5: ("Click the touchpad", {
                "xbox": "View button (the two squares). Hold = hold the touchpad (absorb a power)",
                "switch": "− (minus) button. Hold = hold the touchpad (absorb a power)",
                "8bitdo": "Select button (left of the center). Hold = hold the touchpad"}),
            6: ("Swipe on the touchpad", {
                "xbox": "Hold the View button and move the right stick in the asked direction ('Open Door')",
                "switch": "Hold the − button and move the right stick in the asked direction ('Open Door')",
                "8bitdo": "Hold the Select button and move the right stick in the asked direction ('Open Door')"}),
            7: ("Both ends of the touchpad", {
                "xbox": "Hold the View button and press RB (lift the car off his brother)",
                "switch": "Hold the − button and press R (lift the car off his brother)",
                "8bitdo": "Hold the Select button and press R1 (lift the car off his brother)"}),
        },
    },
}

TRIGGER_TAG = {"xbox": "RT", "switch": "ZR", "8bitdo": "R2"}


def wrap(draw, text, fnt, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        trial = (cur + " " + w).strip()
        if draw.textlength(trial, font=fnt) <= width * SS:
            cur = trial
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def make(kind, lang, src, out, debug=False):
    img = Image.open(src).convert("RGB")
    # crop the white margin so the controller fills the picture; the targets move with the crop
    diff = ImageChops.difference(img, Image.new("RGB", img.size, (255, 255, 255))).convert("L").point(
        lambda v: 255 if v > 28 else 0)
    box = diff.getbbox() or (0, 0, img.size[0], img.size[1])
    m = 30
    box = (max(0, box[0] - m), max(0, box[1] - m), min(img.size[0], box[2] + m), min(img.size[1], box[3] + m))
    img = img.crop(box)
    ox, oy = img.size
    off_x, off_y = box[0], box[1]
    area_w, area_h = 1080, 900
    pad_top = 150
    scale = min((area_w - 80) / ox, (area_h - pad_top - 60) / oy)
    cw, ch = int(ox * scale), int(oy * scale)
    panel_w = 820
    W, H = area_w + panel_w, area_h
    canvas = Image.new("RGB", (W * SS, H * SS), "#ffffff")
    d = ImageDraw.Draw(canvas)
    ix, iy = (area_w - cw) // 2, pad_top + (area_h - pad_top - ch) // 2
    canvas.paste(img.resize((cw * SS, ch * SS), Image.LANCZOS), (ix * SS, iy * SS))
    d.rectangle([area_w * SS, 0, W * SS, H * SS], fill="#121418")

    def px(x, y):
        return (ix + (x - off_x) * scale) * SS, (iy + (y - off_y) * scale) * SS

    tg = TARGETS[kind]
    badge_slots = {}
    for step, items in STEPS[kind].items():
        for name, level in items:
            x, y, r = tg[name]
            cx, cy = px(x, y)
            rr = (r * scale + level * 13) * SS
            wdt = 7 * SS
            if name == "RT":  # trigger: not visible on the picture, drawn as a tag
                bxs, bys, brs = tg["RB"]
                rx, ry = px(bxs, bys)
                d.line([cx, cy, rx, ry], fill=COL[step], width=5 * SS)
                tag = TRIGGER_TAG[kind]
                f = font(26)
                tw = d.textlength(tag, font=f)
                bw, bh = tw + 34 * SS, 46 * SS
                d.rounded_rectangle([cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2], radius=bh / 2,
                                    outline=COL[step], width=wdt, fill="#ffffff")
                d.text((cx, cy), tag, font=f, fill="#111111", anchor="mm")
                bx, by = cx + bw / 2 + 4 * SS, cy - bh / 2 - 4 * SS
            else:
                d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=COL[step], width=wdt)
                n = badge_slots.get(name, 0)
                badge_slots[name] = n + 1
                ang = math.radians(-50 - 55 * n)
                bx, by = cx + math.cos(ang) * rr, cy + math.sin(ang) * rr
            br = 17 * SS
            d.ellipse([bx - br, by - br, bx + br, by + br], fill=COL[step], outline="#ffffff", width=3 * SS)
            d.text((bx, by - 1 * SS), str(step), font=font(22), fill="#101010", anchor="mm")
            if debug:
                d.text((cx, cy + rr + 12 * SS), name, font=font(16), fill="#d00000", anchor="mm")

    # legend
    tx = area_w + 44
    d.text((tx * SS, 52 * SS), TXT[lang]["kicker"], font=font(18), fill="#ff6a2b", anchor="lm")
    d.text((tx * SS, 94 * SS), TXT[lang]["title"], font=font(40), fill="#ffffff", anchor="lm")
    d.text((tx * SS, 132 * SS), TXT[lang]["sub"], font=font(20, False), fill="#a3a9b3", anchor="lm")
    d.text((tx * SS, 166 * SS), NAMES[kind][0 if lang == "pt" else 1], font=font(20), fill="#f5c400", anchor="lm")
    y = 214
    for step in range(1, 8):
        title, descs = TXT[lang]["steps"][step]
        desc = descs[kind]
        d.ellipse([tx * SS, y * SS, (tx + 40) * SS, (y + 40) * SS], fill=COL[step])
        d.text(((tx + 20) * SS, (y + 20) * SS), str(step), font=font(24), fill="#101010", anchor="mm")
        d.text(((tx + 58) * SS, (y + 10) * SS), title, font=font(23), fill="#ffffff", anchor="lm")
        yy = y + 44
        for line in wrap(d, desc, font(17, False), panel_w - 58 - 44 - 10):
            d.text(((tx + 58) * SS, yy * SS), line, font=font(17, False), fill="#b9bec8", anchor="lm")
            yy += 23
        y = max(y + 86, yy + 12)
    d.text((tx * SS, (H - 34) * SS), TXT[lang]["foot"], font=font(16, False), fill="#7d838e", anchor="lm")
    canvas = canvas.resize((W, H), Image.LANCZOS)
    out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out, optimize=True)
    return out


def main():
    src_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    debug = "--debug" in sys.argv
    files = {"xbox": "1.webp", "switch": "2.png", "8bitdo": "3.png"}
    for kind, name in files.items():
        for lang in ("pt", "en"):
            suffix = "-debug" if debug else ""
            out = out_dir / f"ISS-controle-{kind}-{'pt-BR' if lang == 'pt' else 'en-US'}{suffix}.png"
            print(make(kind, lang, src_dir / name, out, debug))


main()
