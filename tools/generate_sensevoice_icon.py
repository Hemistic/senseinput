from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "resources" / "sensevoice.ico"
SIZES = (16, 20, 24, 32, 48, 64, 128, 256)


def draw_icon(size: int) -> Image.Image:
    scale = size / 64.0
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    def rect(box, radius, fill):
        draw.rounded_rectangle(tuple(round(value * scale) for value in box),
                               radius=max(1, round(radius * scale)), fill=fill)

    def polygon(points, fill):
        draw.polygon([(round(x * scale), round(y * scale)) for x, y in points], fill=fill)

    def line(points, fill, width):
        draw.line([(round(x * scale), round(y * scale)) for x, y in points],
                  fill=fill, width=max(1, round(width * scale)), joint="curve")

    rect((3, 3, 61, 61), 17, "#25282E")
    polygon([(17, 14), (47, 14), (57, 24), (57, 37), (47, 47), (31, 47),
             (23, 53), (20, 53), (20, 47), (17, 47), (7, 37), (7, 24)], "#34434A")
    points = [(16, 32), (18, 27), (20, 25), (22, 27), (25, 32),
              (27, 37), (29, 39), (31, 37), (34, 32), (36, 27),
              (38, 25), (40, 27), (43, 32), (45, 37), (47, 39),
              (49, 37), (52, 32)]
    line(points, "#63D9B5", 3.5)
    draw.ellipse((round(13.5 * scale), round(29.5 * scale),
                  round(18.5 * scale), round(34.5 * scale)), fill="#D7F2EA")
    return image


images = [draw_icon(size) for size in SIZES]
OUTPUT.parent.mkdir(parents=True, exist_ok=True)
# Pillow's ICO writer derives all requested sizes from the source image. Use
# the largest raster so Windows does not upscale a 16px-only resource.
images[-1].save(OUTPUT, format="ICO", sizes=[(image.width, image.height) for image in images])
print(OUTPUT)
