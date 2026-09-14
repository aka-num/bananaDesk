#!/usr/bin/env python3
"""Rebuild bananaDesk icons from the supplied artwork; requires Pillow only.

The preserved source is content-detected by Pillow (its original suffix is
.jpeg, although the supplied file contains PNG data). No artwork is redrawn.
Only border-connected white is removed: the white face and headset stay opaque.
"""
from collections import Counter, deque
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

LANCZOS = getattr(Image, "Resampling", Image).LANCZOS


def extract_artwork(source):
    image = Image.open(source).convert("RGB")
    original_size = image.size
    # Remove the narrow dark strip at the right edge of the supplied picture.
    right = image.width
    while right > image.width // 2:
        column = [image.getpixel((right - 1, y)) for y in range(image.height)]
        if sum(max(pixel) < 24 for pixel in column) < image.height * 0.98:
            break
        right -= 1
    image = image.crop((0, 0, right, image.height))
    background = Counter(image.crop((0, 0, image.width, 1)).getdata()).most_common(1)[0][0]
    candidate = Image.new("L", image.size)
    candidate.putdata([0 if min(pixel) >= 230 else 255
                       for pixel in image.getdata()])
    # Flooding only the outside prevents removing enclosed white artwork.
    for seed in ((0, 0), (image.width - 1, 0), (0, image.height - 1), (image.width - 1, image.height - 1)):
        if candidate.getpixel(seed) == 0:
            ImageDraw.floodfill(candidate, seed, 128)
    alpha = candidate.point(lambda value: 0 if value == 128 else 255)
    # Discard isolated background compression speckles; the banana, face and
    # headset form one connected illustration in this source.
    seen = set()
    components = []
    for y in range(image.height):
        for x in range(image.width):
            if not alpha.getpixel((x, y)) or (x, y) in seen:
                continue
            points = []
            pending = deque([(x, y)])
            seen.add((x, y))
            while pending:
                xx, yy = pending.popleft()
                points.append((xx, yy))
                for neighbor in ((xx - 1, yy), (xx + 1, yy), (xx, yy - 1), (xx, yy + 1)):
                    nx, ny = neighbor
                    if 0 <= nx < image.width and 0 <= ny < image.height and neighbor not in seen and alpha.getpixel(neighbor):
                        seen.add(neighbor)
                        pending.append(neighbor)
            components.append(points)
    keep = max(components, key=len)
    alpha = Image.new("L", image.size)
    alpha_pixels = alpha.load()
    for point in keep:
        alpha_pixels[point] = 255

    # Un-matte only the two-pixel outside contour against the original white.
    # Keep all internal pixels unchanged, including the face and microphone.
    solid = alpha.filter(ImageFilter.MinFilter(5))
    outside = alpha.point(lambda value: 255 - value).filter(ImageFilter.MaxFilter(5))
    rgb = image.load()
    matte = alpha.load()
    solid_pixels = solid.load()
    outside_pixels = outside.load()
    offsets = sorted(((dx, dy) for dy in range(-4, 5) for dx in range(-4, 5)
                      if dx or dy), key=lambda p: p[0] * p[0] + p[1] * p[1])
    original = image.copy().load()
    for y in range(image.height):
        for x in range(image.width):
            if not matte[x, y] or not outside_pixels[x, y]:
                continue
            foreground = None
            for dx, dy in offsets:
                xx, yy = x + dx, y + dy
                if 0 <= xx < image.width and 0 <= yy < image.height and solid_pixels[xx, yy] and min(original[xx, yy]) < 200:
                    foreground = original[xx, yy]
                    break
            if foreground is None:
                continue
            channel = max(range(3), key=lambda i: background[i] - foreground[i])
            opacity = (background[channel] - original[x, y][channel]) / (background[channel] - foreground[channel])
            if not 0 < opacity < 0.98:
                continue
            matte[x, y] = round(opacity * 255)
            rgb[x, y] = tuple(max(0, min(255, round((original[x, y][i] - (1 - opacity) * background[i]) / opacity))) for i in range(3))
    rgba = image.convert("RGBA")
    rgba.putalpha(alpha)
    bounds = alpha.getbbox()
    if not bounds:
        raise RuntimeError("No artwork found in the supplied image")
    return rgba.crop(bounds), {"source_size": original_size, "right_border_removed": original_size[0] != right,
                              "right_strip_pixels": original_size[0] - right, "artwork_bounds": bounds}


def make_icon(artwork, size):
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    # Upscale when needed; the supplied artwork itself is approximately 340 px.
    width = round(size * 0.88)
    height = round(width * artwork.height / artwork.width)
    if height > round(size * 0.88):
        height = round(size * 0.88)
        width = round(height * artwork.width / artwork.height)
    content = artwork.resize((width, height), LANCZOS)
    canvas.alpha_composite(content, ((size - width) // 2, (size - height) // 2))
    return canvas


def main():
    assets = Path(__file__).resolve().parent
    artwork, metadata = extract_artwork(assets / "source-icon.jpeg")
    master = make_icon(artwork, 1024)
    master.save(assets / "bananaDesk-1024.png", optimize=True)
    master.resize((512, 512), LANCZOS).save(assets / "bananaDesk.png", optimize=True)
    master.save(assets / "bananaDesk.ico", format="ICO", sizes=[(size, size) for size in (16, 24, 32, 48, 64, 128, 256)])
    print({**metadata, "png_size": [512, 512], "master_size": master.size, "ico_sizes": [16, 24, 32, 48, 64, 128, 256]})


if __name__ == "__main__":
    main()
