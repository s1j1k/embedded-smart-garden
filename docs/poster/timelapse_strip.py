"""Three timelapse photos in a row, each stamped with its capture time.

    python timelapse_strip.py <dir with the original photos>
        -> timelapse_2026-10-05.jpg, timelapse_2026-10-13.jpg, timelapse_2026-10-17.jpg
           and timelapse_strip.jpg

The originals (2304x1296) are on the Pi in data/camera/images/, named by local
capture time. Basil is on the left, lettuce on the right. The camera stand was
moved between 5 and 13 Oct, so the first view is closer.
"""
import sys
from datetime import datetime
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
PHOTOS = ["2026-10-05_16-42-27.jpg", "2026-10-13_11-01-57.jpg", "2026-10-17_11-00-09.jpg"]
W, H = 1280, 720
src = Path(sys.argv[1])


def font(size):
    for name in ("arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


frames = []
for name in PHOTOS:
    when = datetime.strptime(name[:-4], "%Y-%m-%d_%H-%M-%S")
    im = Image.open(src / name).convert("RGB").resize((W, H), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    label = when.strftime("%-d %b %Y, %H:%M") if sys.platform != "win32" else \
        when.strftime("%#d %b %Y, %H:%M")
    f = font(40)
    box = d.textbbox((0, 0), label, font=f)
    d.rectangle((20, H - 30 - (box[3] - box[1]) - 24, 20 + box[2] - box[0] + 32, H - 20),
                fill=(0, 0, 0))
    d.text((36, H - 30 - (box[3] - box[1]) - 12), label, font=f, fill=(255, 255, 255))
    im.save(HERE / f"timelapse_{when:%Y-%m-%d}.jpg", quality=92)
    frames.append(im)

gap = 24
strip = Image.new("RGB", (3 * W + 2 * gap, H), "white")
for i, im in enumerate(frames):
    strip.paste(im, (i * (W + gap), 0))
strip.save(HERE / "timelapse_strip.jpg", quality=92)
