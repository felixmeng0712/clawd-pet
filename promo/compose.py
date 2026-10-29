"""Compose captured device frames into a 1920x1080 promo video (frames piped to ffmpeg)."""
import os, sys, subprocess, json
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
W, H, FPS = 1920, 1080, 25
XF = 12  # crossfade frames

AV = "/System/Library/Fonts/Avenir Next.ttc"
ME = "/System/Library/Fonts/Menlo.ttc"
F_TITLE = ImageFont.truetype(AV, 64, index=2)
F_BODY = ImageFont.truetype(AV, 30, index=5)
F_KICK = ImageFont.truetype(ME, 22, index=1)
F_LOG = ImageFont.truetype(ME, 21, index=0)
F_HERO = ImageFont.truetype(AV, 120, index=0)
F_HERO_SUB = ImageFont.truetype(AV, 36, index=5)
F_SMALL = ImageFont.truetype(AV, 26, index=5)

BG = np.array([17, 15, 14], np.float32)
ORANGE = (217, 119, 87)
WHITE = (240, 236, 230)
GREY = (150, 144, 138)
DIM = (95, 90, 85)

# ---- device geometry ----
SCALE = 3
SW, SH = 320 * SCALE, 240 * SCALE
DX, DY = 70, 100                    # device body top-left
BW, BH = SW + 80, SH + 150          # body size
SX, SY = DX + 40, DY + 40            # screen top-left


def frames(name):
    d = f"{HERE}/frames/{name}"
    return [f"{d}/{f}" for f in sorted(os.listdir(d))]


def build_static():
    """Background + device body (without screen) and a glow mask."""
    base = np.ones((H, W, 3), np.float32) * BG
    # soft vignette
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    v = 1 - 0.35 * (((xx - W / 2) / W) ** 2 + ((yy - H / 2) / H) ** 2) * 2.2
    base *= v[..., None]
    img = Image.fromarray(base.clip(0, 255).astype(np.uint8))
    d = ImageDraw.Draw(img)
    # drop shadow
    sh = Image.new("L", (W, H), 0)
    ImageDraw.Draw(sh).rounded_rectangle([DX + 10, DY + 30, DX + BW + 10, DY + BH + 40], 56, fill=170)
    sh = sh.filter(ImageFilter.GaussianBlur(40))
    img = Image.composite(Image.new("RGB", (W, H), (0, 0, 0)), img, sh)
    d = ImageDraw.Draw(img)
    # body: graphite with a lighter rim
    d.rounded_rectangle([DX, DY, DX + BW, DY + BH], 48, fill=(58, 58, 62))
    d.rounded_rectangle([DX + 4, DY + 4, DX + BW - 4, DY + BH - 4], 45, fill=(40, 40, 44))
    # bezel around the screen
    d.rounded_rectangle([SX - 14, SY - 14, SX + SW + 14, SY + SH + 14], 14, fill=(10, 10, 12))
    # little "M5" style label + three touch dots under the screen
    cy = SY + SH + 50
    for k in (-1, 0, 1):
        cx = DX + BW // 2 + k * 160
        d.ellipse([cx - 7, cy - 7, cx + 7, cy + 7], fill=(70, 70, 76))
    # glow mask (screen-shaped, very blurred)
    g = Image.new("L", (W, H), 0)
    ImageDraw.Draw(g).rounded_rectangle([SX - 40, SY - 40, SX + SW + 40, SY + SH + 40], 40, fill=255)
    g = g.filter(ImageFilter.GaussianBlur(120))
    glow = np.asarray(g, np.float32)[..., None] / 255.0
    return np.asarray(img, np.float32), glow


STATIC, GLOW = build_static()


def ease(t):
    t = min(max(t, 0.0), 1.0)
    return 1 - (1 - t) ** 3


def text_layer(draw_fn):
    """Render RGBA text layer via callback(draw)."""
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(layer))
    return layer


def alpha_over(base_u8, layer, alpha=1.0, dy=0):
    if dy:
        layer = layer.transform(layer.size, Image.AFFINE, (1, 0, 0, 0, 1, -dy))
    if alpha < 1:
        a = layer.getchannel("A").point(lambda p: int(p * alpha))
        layer.putalpha(a)
    base_u8.alpha_composite(layer)


def wrap(draw, text, font, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if draw.textlength(t, font=font) > width and cur:
            lines.append(cur)
            cur = w
        else:
            cur = t
    lines.append(cur)
    return lines


TX = DX + BW + 80   # text column
TW = W - TX - 70


def caption_layer(kicker, title, body):
    end = [0]

    def f(d):
        y = 250
        d.text((TX, y), kicker, font=F_KICK, fill=ORANGE)
        y += 48
        for line in wrap(d, title, F_TITLE, TW):
            d.text((TX, y), line, font=F_TITLE, fill=WHITE)
            y += 88
        y += 18
        for line in wrap(d, body, F_BODY, TW):
            d.text((TX, y), line, font=F_BODY, fill=GREY)
            y += 44
        end[0] = y
    layer = text_layer(f)
    return layer, end[0]


def log_layer(lines, newest_age, top):
    """Terminal-ish hook log; newest line highlighted and sliding in."""
    def f(d):
        x0, y0, w = TX + 24, top + 70, TW - 24
        d.rounded_rectangle([x0 - 24, y0 - 22, x0 + w, y0 + 4 * 38 + 10], 16, fill=(28, 26, 25, 235), outline=(52, 49, 46, 255))
        show = lines[-4:]
        for k, (a, b) in enumerate(show):
            newest = k == len(show) - 1
            t = ease(newest_age / 8) if newest else 1
            y = y0 + k * 38
            col_a = ORANGE if newest else DIM
            col_b = WHITE if newest else DIM
            xo = int((1 - t) * 30)
            alpha = int(255 * t)
            d.text((x0 + xo, y), a, font=F_LOG, fill=col_a + (alpha,))
            d.text((x0 + xo + d.textlength(a + "  ", font=F_LOG), y), b, font=F_LOG, fill=col_b + (alpha,))
    return text_layer(f)


def device_frame(screen_png, cap_alpha, caption, log, log_age):
    scr = Image.open(screen_png).convert("RGB")
    mean = np.asarray(scr, np.float32).reshape(-1, 3).mean(0)
    arr = STATIC + GLOW * mean * 0.45
    img = Image.fromarray(arr.clip(0, 255).astype(np.uint8)).convert("RGBA")
    img.paste(scr.resize((SW, SH), Image.NEAREST), (SX, SY))
    caption, top = caption
    alpha_over(img, caption, cap_alpha, dy=int((1 - cap_alpha) * 24))
    if log:
        alpha_over(img, log_layer(log, log_age, top), cap_alpha)
    return img.convert("RGB")


def hero_frame(screen_png, lines, a):
    """Small sharp screen with a glow, big centered text underneath."""
    scr = Image.open(screen_png).convert("RGB")
    mean = np.asarray(scr, np.float32).reshape(-1, 3).mean(0)
    img = Image.fromarray((HERO_BG + HERO_GLOW * mean * 0.5).clip(0, 255).astype(np.uint8)).convert("RGBA")
    img.paste(scr.resize((HW, HH), Image.NEAREST), (HX, HY), HERO_MASK)

    def f(d):
        y = lines[0]
        for font, text, col in lines[1]:
            tw = d.textlength(text, font=font)
            d.text(((W - tw) / 2, y), text, font=font, fill=col)
            y += font.size * 1.35
    alpha_over(img, text_layer(f), a, dy=int((1 - a) * 30))
    return img.convert("RGB")


HW, HH = 640, 480
HX, HY = (W - HW) // 2, 120
HERO_MASK = Image.new("L", (HW, HH), 0)
ImageDraw.Draw(HERO_MASK).rounded_rectangle([0, 0, HW - 1, HH - 1], 18, fill=255)
_g = Image.new("L", (W, H), 0)
ImageDraw.Draw(_g).rounded_rectangle([HX, HY, HX + HW, HY + HH], 30, fill=255)
HERO_GLOW = np.asarray(_g.filter(ImageFilter.GaussianBlur(110)), np.float32)[..., None] / 255.0
_yy, _xx = np.mgrid[0:H, 0:W].astype(np.float32)
HERO_BG = (np.ones((H, W, 3), np.float32) * BG) * (1 - 0.35 * (((_xx - W / 2) / W) ** 2 + ((_yy - H / 2) / H) ** 2) * 2.2)[..., None]


# ---------------- storyboard ----------------
LOG_FLOW = {0: ("UserPromptSubmit", "→ thinking"),
            55: ("PreToolUse Bash", "→ working"),
            115: ("PreToolUse Edit", "→ working"),
            170: ("PermissionRequest", "→ waiting  ♪ beep"),
            240: ("Stop", "→ done")}
LOG_MULTI = {0: ("session my-app", "→ working"),
             40: ("session api", "→ thinking"),
             85: ("session docs", "→ waiting")}
LOG_TOUCH = {10: ("tap eyes", "→ shy"),
             60: ("long press", "→ tickle"),
             115: ("4 quick taps", "→ angry")}
LOG_DAY = {0: ("05:36", "dawn"), 50: ("12:00", "day"), 100: ("18:00", "dusk"), 150: ("21:30", "night")}
LOG_WX = {0: ("weather", "rain"), 75: ("weather", "snow"), 150: ("weather", "thunderstorm")}

SEGMENTS = [
    ("hero", "pet", 0, 85, [650, [(F_HERO, "Clawd", WHITE),
                                   (F_HERO_SUB, "a tiny desk pet for Claude Code", GREY)]]),
    ("dev", "pet", 0, 110, ("01 · MEET CLAWD", "A pixel crab on your desk",
                             "It lives on a little beach on an M5Stack screen and naps when nothing is going on."), None),
    ("dev", "flow", 0, 300, ("02 · LIVE STATUS", "Watches Claude work",
                              "Claude Code hooks push every step over Wi-Fi. When Claude needs you, it beeps."), LOG_FLOW),
    ("dev", "multi", 0, 190, ("03 · SESSIONS", "One crab per session",
                               "Up to three gather around the campfire. New sessions walk in from the edge."), LOG_MULTI),
    ("dev", "day", 0, 200, ("04 · REAL TIME", "Follows the sun",
                             "Dawn, day, dusk and night, blended in real time with a clock in the corner."), LOG_DAY),
    ("dev", "weather", 0, 225, ("05 · WEATHER", "And the sky outside",
                                 "Live local weather: rain, snow, fog and thunderstorms with lightning."), LOG_WX),
    ("dev", "touch", 0, 170, ("06 · TOUCH", "Pet it",
                               "Tap its eyes and it blushes. Hold to tickle. Poke it too much and it gets mad."), LOG_TOUCH),
    ("hero", "multi", 100, 110, [650, [(F_HERO, "Clawd", WHITE),
                                       (F_HERO_SUB, "M5Stack CoreS3  ·  ESP32  ·  Claude Code hooks", GREY),
                                       (F_SMALL, " ", GREY),
                                       (F_SMALL, "Unofficial fan project. Clawd is Anthropic's mascot.", DIM)]]),
]


def render_segment(seg):
    kind = seg[0]
    if kind == "hero":
        _, scene, start, n, spec = seg
        fs = frames(scene)
        for i in range(n):
            png = fs[min(start + i, len(fs) - 1)]
            yield hero_frame(png, spec, ease((i - 6) / 18))
        return
    _, scene, start, n, cap, log = seg
    fs = frames(scene)
    caption = caption_layer(*cap)
    for i in range(n):
        lines, age = [], 0
        if log:
            keys = [k for k in sorted(log) if k <= i]
            lines = [log[k] for k in keys]
            age = i - keys[-1] if keys else 0
        yield device_frame(fs[min(start + i, len(fs) - 1)], ease((i - 4) / 16), caption, lines, age)


def main(out, preview_only=None):
    if preview_only is not None:
        seg = SEGMENTS[preview_only]
        for k, im in enumerate(render_segment(seg)):
            if k == seg[3] // 2:
                im.save(f"{HERE}/preview_{preview_only}.png")
                return
    ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                           "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
                           "-c:v", "libx264", "-preset", "slow", "-crf", "16", "-pix_fmt", "yuv420p", out],
                          stdin=subprocess.PIPE)
    total = 0
    tail = []  # last XF frames of previous segment, held back for crossfade
    for si, seg in enumerate(SEGMENTS):
        gen = render_segment(seg)
        buf = []
        for k, im in enumerate(gen):
            a = np.asarray(im, np.float32)
            if k < len(tail):
                t = (k + 1) / (len(tail) + 1)
                a = tail[k] * (1 - t) + a * t
            buf.append(a)
            if len(buf) > XF:
                ff.stdin.write(buf.pop(0).astype(np.uint8).tobytes())
                total += 1
        if si == len(SEGMENTS) - 1:
            for k, a in enumerate(buf):  # fade to black at the very end
                ff.stdin.write((a * (1 - (k + 1) / len(buf))).astype(np.uint8).tobytes())
                total += 1
        tail = buf
        print(seg[1], "done", total, flush=True)
    ff.stdin.close()
    ff.wait()
    json.dump({"frames": total}, open(f"{HERE}/meta.json", "w"))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "preview":
        main(None, int(sys.argv[2]))
    else:
        main(f"{HERE}/video_silent.mp4")
