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
    polygon([(16, 18), (48, 18), (56, 26), (56, 36), (48, 44), (32, 44),
             (22.8, 51.2), (19.6, 50.4), (19.6, 44), (16, 44), (8, 36),
             (8, 26)], "#34434A")
    for x, top, bottom in ((18, 32, 40), (25, 27, 45), (32, 23, 49),
                           (39, 27, 45), (46, 30, 42)):
        rect((x, top, x + 4, bottom), 1.5, "#63D9B5")
    line(((17, 50), (47, 50)), "#D7F2EA", 3)
    draw.ellipse((round(14.5 * scale), round(47.5 * scale),
                  round(19.5 * scale), round(52.5 * scale)), fill="#63D9B5")
    return image


images = [draw_icon(size) for size in SIZES]
OUTPUT.parent.mkdir(parents=True, exist_ok=True)
# Pillow's ICO writer derives all requested sizes from the source image. Use
# the largest raster so Windows does not upscale a 16px-only resource.
images[-1].save(OUTPUT, format="ICO", sizes=[(image.width, image.height) for image in images])
print(OUTPUT)
