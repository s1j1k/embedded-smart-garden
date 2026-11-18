"""Put dashboard screenshots in simple device frames for the poster.

    python frame_screens.py phone screens/phone_home_raw.jpg phone_home.png
    python frame_screens.py browser screens/desktop_trends_raw.png desktop_trends.png

phone: rounded dark bezel, camera notch, side buttons, soft shadow.
browser: window with title bar, three dots, address bar showing the dashboard URL.
Transparent background (PNG) so the frame sits on any poster colour.
"""
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

BEZEL = (28, 31, 36, 255)
FRAME_EDGE = (70, 76, 86, 255)


def _font(size):
    for name in ("segoeui.ttf", "arial.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def _shadow(size, box, radius, blur, offset=(0, 18), alpha=90):
    sh = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(sh)
    x0, y0, x1, y1 = box
    d.rounded_rectangle((x0 + offset[0], y0 + offset[1], x1 + offset[0], y1 + offset[1]),
                        radius, fill=(0, 0, 0, alpha))
    return sh.filter(ImageFilter.GaussianBlur(blur))


def phone(src, out, screen_w=750):
    shot = Image.open(src).convert("RGBA")
    scale = screen_w / shot.width
    shot = shot.resize((screen_w, int(shot.height * scale)), Image.LANCZOS)
    sw, sh = shot.size
    bezel = int(screen_w * 0.045)
    r_out, r_in = int(screen_w * 0.14), int(screen_w * 0.10)
    margin = 90
    W, H = sw + 2 * bezel + 2 * margin, sh + 2 * bezel + 2 * margin
    body = (margin, margin, margin + sw + 2 * bezel, margin + sh + 2 * bezel)
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    canvas.alpha_composite(_shadow((W, H), body, r_out, 28))
    d = ImageDraw.Draw(canvas)
    # side buttons
    bx = body[2] - 2
    d.rounded_rectangle((bx, body[1] + 260, bx + 10, body[1] + 400), 5, fill=FRAME_EDGE)
    d.rounded_rectangle((body[0] - 8, body[1] + 220, body[0] + 2, body[1] + 300), 5, fill=FRAME_EDGE)
    d.rounded_rectangle((body[0] - 8, body[1] + 320, body[0] + 2, body[1] + 400), 5, fill=FRAME_EDGE)
    d.rounded_rectangle(body, r_out, fill=BEZEL, outline=FRAME_EDGE, width=4)
    # screen with rounded corners
    mask = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, sw - 1, sh - 1), r_in, fill=255)
    canvas.paste(shot, (body[0] + bezel, body[1] + bezel), mask)
    # camera notch (pill)
    cx = (body[0] + body[2]) // 2
    nw, nh = int(screen_w * 0.28), int(screen_w * 0.06)
    top = body[1] + bezel + 14
    d.rounded_rectangle((cx - nw // 2, top, cx + nw // 2, top + nh), nh // 2, fill=BEZEL)
    canvas.save(out)


def browser(src, out, url="smart-garden.local:8501", width=1600):
    shot = Image.open(src).convert("RGBA")
    scale = width / shot.width
    shot = shot.resize((width, int(shot.height * scale)), Image.LANCZOS)
    sw, sh = shot.size
    bar = 76
    radius = 22
    margin = 70
    W, H = sw + 2 * margin, sh + bar + 2 * margin
    win = (margin, margin, margin + sw, margin + bar + sh)
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    canvas.alpha_composite(_shadow((W, H), win, radius, 26, offset=(0, 14), alpha=70))
    d = ImageDraw.Draw(canvas)
    d.rounded_rectangle(win, radius, fill=(236, 239, 243, 255), outline=(205, 210, 218, 255), width=2)
    # traffic-light dots
    for i, col in enumerate(((255, 95, 87), (255, 189, 46), (40, 201, 64))):
        x = win[0] + 30 + i * 30
        d.ellipse((x, win[1] + 28, x + 18, win[1] + 46), fill=col)
    # address bar
    ax0, ax1 = win[0] + 150, win[2] - 150
    d.rounded_rectangle((ax0, win[1] + 18, ax1, win[1] + 58), 20, fill=(255, 255, 255, 255))
    f = _font(24)
    d.text((ax0 + 26, win[1] + 24), "\U0001F512 " + url if False else url, font=f, fill=(90, 96, 105))
    # page, rounded bottom corners only
    mask = Image.new("L", (sw, sh), 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle((0, -radius, sw - 1, sh - 1), radius, fill=255)
    canvas.paste(shot, (win[0], win[1] + bar), mask)
    canvas.save(out)


def crop(src, top, height, tmp):
    """Crop a tall page capture to a device-shaped slice (pixels of the raw image)."""
    im = Image.open(src)
    im.crop((0, top, im.width, min(im.height, top + height))).save(tmp)
    return tmp


if __name__ == "__main__":
    kind, src, out = sys.argv[1], sys.argv[2], sys.argv[3]
    if len(sys.argv) > 5:          # optional crop: top height (raw pixels)
        src = crop(src, int(sys.argv[4]), int(sys.argv[5]), out + ".crop.png")
    {"phone": phone, "browser": browser}[kind](src, out)
    if len(sys.argv) > 5:
        import os
        os.remove(src)
    print("wrote", out)
