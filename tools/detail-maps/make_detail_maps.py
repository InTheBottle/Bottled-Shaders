#!/usr/bin/env python3
"""
make_detail_maps.py — Build tiling micro-detail textures from CC0 ambientCG materials.

Each set in sets.json is downloaded once, high-passed so only fine detail survives,
normalised to shared strength targets, packed and encoded as BC7 with a full mip chain:

    R  albedo multiplier / 2      (0.5 = unchanged; shader: baseColor *= 2 * R)
    G  tangent normal Y           (DirectX convention, matches Skyrim normal maps)
    B  roughness offset / 2 + 0.5 (0.5 = unchanged; shader: roughness += 2 * (B - 0.5))
    A  tangent normal X

Usage:
    python tools/detail-maps/make_detail_maps.py --texconv <path to texconv.exe>
    python tools/detail-maps/make_detail_maps.py --texconv <exe> --out "packs/Detail Maps Pack/Shaders/DetailMaps/Sets"
    python tools/detail-maps/make_detail_maps.py --texconv <exe> --fast --only metal stone
    python tools/detail-maps/make_detail_maps.py --png-only --preview build/detail-maps/preview.png
"""

import argparse
import io
import json
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

USER_AGENT = "BottledShaders-detailmaps/1.0"
MAPS = ("Color", "NormalDX", "Roughness", "AmbientOcclusion", "Displacement")
MIN_NORMAL_SLOPE_RMS = 0.03


def find_project_root():
    d = Path(__file__).resolve().parent
    while d != d.parent:
        if (d / "CMakeLists.txt").exists():
            return d
        d = d.parent
    return Path.cwd()


def fetch(asset, resolution, cache):
    folder = cache / asset
    if all((folder / f"{asset}_{resolution}-PNG_{m}.png").exists() for m in MAPS[:3]):
        return folder
    folder.mkdir(parents=True, exist_ok=True)
    url = f"https://ambientcg.com/get?file={asset}_{resolution}-PNG.zip"
    print(f"  downloading {url}")
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": USER_AGENT}), timeout=300) as r:
        blob = r.read()
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        for info in z.infolist():
            name = info.filename.rsplit("/", 1)[-1]
            if not info.is_dir() and any(name == f"{asset}_{resolution}-PNG_{m}.png" for m in MAPS):
                (folder / name).write_bytes(z.read(info))
    return folder


def load(folder, asset, resolution, kind):
    path = folder / f"{asset}_{resolution}-PNG_{kind}.png"
    if not path.exists():
        return None
    image = Image.open(path)
    data = np.asarray(image, dtype=np.float64)
    return data / (65535.0 if image.mode.startswith("I;16") else 255.0)


def lowpass(x, sigma):
    h, w = x.shape[:2]
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.rfftfreq(w)[None, :]
    kernel = np.exp(-2.0 * np.pi**2 * sigma**2 * (fx**2 + fy**2))
    if x.ndim == 2:
        return np.fft.irfft2(np.fft.rfft2(x) * kernel, s=(h, w))
    return np.stack([np.fft.irfft2(np.fft.rfft2(x[..., c]) * kernel, s=(h, w)) for c in range(x.shape[2])], axis=-1)


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def build_set(entry, config, cache):
    asset = entry["asset"]
    resolution = config["resolution"]
    targets = config["targets"]
    folder = fetch(asset, resolution, cache)
    color = load(folder, asset, resolution, "Color")
    normal = load(folder, asset, resolution, "NormalDX")
    rough = load(folder, asset, resolution, "Roughness")
    ao = load(folder, asset, resolution, "AmbientOcclusion")
    if color is None or normal is None or rough is None:
        raise RuntimeError(f"{asset}: missing Color, NormalDX or Roughness")
    if color.ndim == 2:
        color = np.repeat(color[..., None], 3, axis=-1)
    if rough.ndim == 3:
        rough = rough[..., 0]
    if ao is not None and ao.ndim == 3:
        ao = ao[..., 0]
    sigma = entry.get("highpassFraction", config["highpassFraction"]) * color.shape[1]

    luminance = srgb_to_linear(color[..., :3]) @ np.array([0.2126, 0.7152, 0.0722])
    if ao is not None:
        luminance = luminance * ao
    logL = np.log(np.maximum(luminance, 1e-4))
    detail = logL - lowpass(logL, sigma)
    detail *= targets["albedoLogStd"] * entry["albedo"] / max(detail.std(), 1e-6)
    ratio = np.exp(detail)
    ratio /= ratio.mean()
    red = np.clip(0.5 * ratio, 0.0, 1.0)

    roughDetail = rough - lowpass(rough, sigma)
    roughDetail *= targets["roughnessStd"] * entry["roughness"] / max(roughDetail.std(), 1e-6)
    blue = np.clip(0.5 + 0.5 * roughDetail, 0.0, 1.0)

    n = normal[..., :3] * 2.0 - 1.0
    nz = np.maximum(n[..., 2], 0.05)
    slope = n[..., :2] / nz[..., None]
    slope = slope - lowpass(slope, sigma)
    rms = np.sqrt(np.mean(np.sum(slope**2, axis=-1)))
    normalSource = entry.get("normalSource", "displacement" if rms < MIN_NORMAL_SLOPE_RMS else "normal")
    if normalSource == "displacement":
        height = load(folder, asset, resolution, "Displacement")
        if height is None:
            raise RuntimeError(f"{asset}: normal map too weak and no displacement to derive from")
        if height.ndim == 3:
            height = height[..., 0]
        du = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
        dv = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
        slope = np.stack([-du, -dv], axis=-1)
        slope = slope - lowpass(slope, sigma)
        rms = np.sqrt(np.mean(np.sum(slope**2, axis=-1)))
    slope *= targets["slopeRms"] * entry["normal"] / max(rms, 1e-6)
    out = np.concatenate([slope, np.ones_like(slope[..., :1])], axis=-1)
    out /= np.linalg.norm(out, axis=-1, keepdims=True)

    packed = np.stack([red, out[..., 1] * 0.5 + 0.5, blue, out[..., 0] * 0.5 + 0.5], axis=-1)
    stats = {"albedoStd": float(red.std() * 2), "roughStd": float(roughDetail.std()), "slopeRms": float(targets["slopeRms"] * entry["normal"]), "normalFrom": normalSource, "ao": ao is not None}
    return np.round(packed * 255.0).astype(np.uint8), stats


def shade(packed, tiles=2):
    p = np.tile(packed.astype(np.float64) / 255.0, (tiles, tiles, 1))
    nx = p[..., 3] * 2 - 1
    ny = p[..., 1] * 2 - 1
    nz = np.sqrt(np.clip(1 - nx**2 - ny**2, 0, 1))
    n = np.stack([nx, ny, nz], axis=-1)
    light = np.array([-0.55, -0.55, 0.63])
    light /= np.linalg.norm(light)
    half = light + np.array([0.0, 0.0, 1.0])
    half /= np.linalg.norm(half)
    albedo = 0.45 * 2 * p[..., 0]
    roughness = np.clip(0.55 + 2 * (p[..., 2] - 0.5), 0.05, 1.0)
    power = 2.0 / roughness**4 - 2.0
    spec = 0.25 * np.clip(n @ half, 0, 1) ** power
    lit = albedo * np.clip(n @ light, 0, 1) + spec + 0.08 * albedo
    return np.clip(lit, 0, 1) ** (1 / 2.2)


def preview(results, path, cell=384):
    from PIL import ImageDraw

    rows = []
    for name, packed in results:
        nrm = np.stack([packed[..., 3], packed[..., 1], np.full(packed.shape[:2], 255, np.uint8)], axis=-1)
        panels = [
            Image.fromarray(packed[..., 0]).convert("RGB"),
            Image.fromarray(nrm),
            Image.fromarray(packed[..., 2]).convert("RGB"),
            Image.fromarray((shade(packed) * 255).astype(np.uint8)),
        ]
        row = Image.new("RGB", (cell * 4, cell))
        for i, panel in enumerate(panels):
            row.paste(panel.resize((cell, cell), Image.LANCZOS), (i * cell, 0))
        draw = ImageDraw.Draw(row)
        draw.rectangle([0, 0, cell * 4, 16], fill=(0, 0, 0))
        draw.text((4, 2), f"{name}:  albedo (R)  |  normal (A,G)  |  roughness offset (B)  |  lit, 2x2 tiles", fill=(255, 255, 0))
        rows.append(row)
    sheet = Image.new("RGB", (cell * 4, cell * len(rows)))
    for i, row in enumerate(rows):
        sheet.paste(row, (0, i * cell))
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)


def main():
    root = find_project_root()
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", type=Path, default=here / "sets.json")
    parser.add_argument("--cache", type=Path, default=root / "build" / "detail-maps" / "cache")
    parser.add_argument("--out", type=Path, default=root / "build" / "detail-maps" / "dds")
    parser.add_argument("--texconv", type=Path, default=None)
    parser.add_argument("--png-only", action="store_true")
    parser.add_argument("--fast", action="store_true", help="GPU BC7 encode (seconds) instead of exhaustive CPU (about a minute per set)")
    parser.add_argument("--preview", type=Path, default=None)
    parser.add_argument("--only", nargs="*", default=None)
    args = parser.parse_args()

    config = json.loads(args.config.read_text(encoding="utf-8"))
    texconv = args.texconv or (Path(shutil.which("texconv")) if shutil.which("texconv") else None)
    if not args.png_only and (texconv is None or not texconv.exists()):
        sys.exit("texconv.exe not found: pass --texconv <path> or use --png-only")

    pngDir = args.out if args.png_only else root / "build" / "detail-maps" / "png"
    pngDir.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)

    results = []
    for entry in config["sets"]:
        if args.only and entry["name"] not in args.only:
            continue
        print(f"{entry['name']} <- {entry['asset']}")
        packed, stats = build_set(entry, config, args.cache)
        print("  " + ", ".join(f"{k}={v:.3f}" if isinstance(v, float) else f"{k}={v}" for k, v in stats.items()))
        png = pngDir / f"{entry['name']}.png"
        Image.fromarray(packed, "RGBA").save(png)
        results.append((entry["name"], packed))
        if not args.png_only:
            quality = [] if args.fast else ["-nogpu", "-bc", "x"]
            subprocess.run([str(texconv), "-nologo", "-y", "-f", "BC7_UNORM", "-m", "0", "-sepalpha", *quality, "-o", str(args.out), str(png)], check=True, stdout=subprocess.DEVNULL)
            print(f"  wrote {args.out / (entry['name'] + '.dds')}")

    if args.preview:
        preview(results, args.preview)
        print(f"preview: {args.preview}")


if __name__ == "__main__":
    main()
