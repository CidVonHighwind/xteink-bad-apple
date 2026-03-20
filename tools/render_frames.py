#!/usr/bin/env python3
"""
Convert a video file to 1bpp PackBits-compressed frames for the e-ink video player.

Output: <out>/video.mrv

.mrv file format:
  Bytes 0-3:   "MRVD" magic
  Bytes 4-7:   frame_count (uint32 LE)
  Bytes 8-...: (frame_count+1) x uint32 LE byte offsets from start of file
  Then:        PackBits-compressed 1bpp frame data

Each uncompressed frame is exactly FRAME_BYTES bytes: 1bpp MSB-first,
0=black 1=white, 800x480 pixels.

Requirements: pip install Pillow  (ffmpeg must be on PATH)
"""

import argparse
import json
import os
import struct
import subprocess
import sys
from pathlib import Path
from typing import Optional

EPD_WIDTH = 800
EPD_HEIGHT = 480
FRAME_BYTES = (EPD_WIDTH // 8) * EPD_HEIGHT  # 48000

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow not found. Run: pip install Pillow")


def encode_packbits(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        run_val = data[i]
        j = i + 1
        while j < n and j - i < 128 and data[j] == run_val:
            j += 1
        run = j - i
        if run >= 2:
            out.append((257 - run) & 0xFF)
            out.append(run_val)
            i = j
        else:
            lit_start = i
            i += 1
            while i < n and i - lit_start < 128:
                if i + 1 < n and data[i] == data[i + 1]:
                    break
                i += 1
            count = i - lit_start
            out.append(count - 1)
            out.extend(data[lit_start:i])
    return bytes(out)


def get_video_dimensions(video_path: str):
    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height",
            "-of",
            "json",
            video_path,
        ],
        capture_output=True,
        text=True,
    )
    info = json.loads(result.stdout)
    return info["streams"][0]["width"], info["streams"][0]["height"]


def fit_dimensions(src_w: int, src_h: int, max_w: int, max_h: int):
    scale = min(max_w / src_w, max_h / src_h)
    return max(1, int(src_w * scale)), max(1, int(src_h * scale))


def mirror_pad(img: Image.Image, target_w: int, target_h: int) -> Image.Image:
    src_w, src_h = img.size
    canvas = Image.new("L", (target_w, target_h), 255)
    x0 = (target_w - src_w) // 2
    y0 = (target_h - src_h) // 2
    canvas.paste(img, (x0, y0))
    blt = x0
    brg = target_w - x0 - src_w
    btp = y0
    bbt = target_h - y0 - src_h
    if blt > 0:
        canvas.paste(
            img.crop((0, 0, blt, src_h)).transpose(Image.FLIP_LEFT_RIGHT), (0, y0)
        )
    if brg > 0:
        canvas.paste(
            img.crop((src_w - brg, 0, src_w, src_h)).transpose(Image.FLIP_LEFT_RIGHT),
            (x0 + src_w, y0),
        )
    if btp > 0:
        canvas.paste(
            img.crop((0, 0, src_w, btp)).transpose(Image.FLIP_TOP_BOTTOM), (x0, 0)
        )
    if bbt > 0:
        canvas.paste(
            img.crop((0, src_h - bbt, src_w, src_h)).transpose(Image.FLIP_TOP_BOTTOM),
            (x0, y0 + src_h),
        )
    if blt > 0 and btp > 0:
        canvas.paste(
            img.crop((0, 0, blt, btp))
            .transpose(Image.FLIP_LEFT_RIGHT)
            .transpose(Image.FLIP_TOP_BOTTOM),
            (0, 0),
        )
    if brg > 0 and btp > 0:
        canvas.paste(
            img.crop((src_w - brg, 0, src_w, btp))
            .transpose(Image.FLIP_LEFT_RIGHT)
            .transpose(Image.FLIP_TOP_BOTTOM),
            (x0 + src_w, 0),
        )
    if blt > 0 and bbt > 0:
        canvas.paste(
            img.crop((0, src_h - bbt, blt, src_h))
            .transpose(Image.FLIP_LEFT_RIGHT)
            .transpose(Image.FLIP_TOP_BOTTOM),
            (0, y0 + src_h),
        )
    if brg > 0 and bbt > 0:
        canvas.paste(
            img.crop((src_w - brg, src_h - bbt, src_w, src_h))
            .transpose(Image.FLIP_LEFT_RIGHT)
            .transpose(Image.FLIP_TOP_BOTTOM),
            (x0 + src_w, y0 + src_h),
        )
    return canvas


def video_to_mrv(
    video_path: str,
    out_dir: str,
    fps: float,
    white_point: int = 220,
    preview_every: int = 0,
    start: Optional[str] = None,
    end: Optional[str] = None,
) -> int:
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    vid_w, vid_h = fit_dimensions(
        *get_video_dimensions(video_path), EPD_WIDTH, EPD_HEIGHT
    )
    bx = (EPD_WIDTH - vid_w) // 2
    by = (EPD_HEIGHT - vid_h) // 2
    print(
        f"  Video: {vid_w}x{vid_h}  (L/R border: {bx}px  T/B border: {by}px)  mirror-padded"
    )

    if preview_every > 0:
        preview_dir = os.path.join(out_dir, "preview")
        Path(preview_dir).mkdir(exist_ok=True)

    cmd = ["ffmpeg"]
    if start:
        cmd += ["-ss", start]
    cmd += ["-i", video_path]
    if end:
        cmd += ["-to", end]
    cmd += [
        "-vf",
        f"fps={fps},scale={vid_w}:{vid_h}",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "-",
    ]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    frame_bytes_rgb = vid_w * vid_h * 3
    frame_num = 0
    compressed_frames = []

    lut = [255 if x >= white_point else x for x in range(256)]

    while True:
        raw = proc.stdout.read(frame_bytes_rgb)
        if len(raw) < frame_bytes_rgb:
            break

        img = Image.frombytes("RGB", (vid_w, vid_h), raw)
        gray = mirror_pad(img.convert("L"), EPD_WIDTH, EPD_HEIGHT).point(lut)
        bw = gray.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
        raw_1bpp = bw.tobytes()
        assert len(raw_1bpp) == FRAME_BYTES

        compressed_frames.append(encode_packbits(raw_1bpp))

        if preview_every > 0 and frame_num % preview_every == 0:
            bw.convert("L").save(
                os.path.join(preview_dir, f"frame_{frame_num:05d}.png")
            )

        frame_num += 1
        if frame_num % 50 == 0:
            print(f"  {frame_num} frames...", end="\r", flush=True)

    proc.wait()
    if proc.returncode not in (0, None) and frame_num == 0:
        sys.exit(
            "ffmpeg failed with no frames produced. Check the video path and ffmpeg installation."
        )

    if frame_num == 0:
        return 0

    out_file = os.path.join(out_dir, "video.mrv")
    header_size = 4 + 4 + (frame_num + 1) * 4
    with open(out_file, "wb") as f:
        f.write(b"MRVD")
        f.write(struct.pack("<I", frame_num))
        pos = header_size
        offsets = []
        for cf in compressed_frames:
            offsets.append(pos)
            pos += len(cf)
        offsets.append(pos)
        for off in offsets:
            f.write(struct.pack("<I", off))
        for cf in compressed_frames:
            f.write(cf)

    total_comp = sum(len(cf) for cf in compressed_frames) + header_size
    ratio = FRAME_BYTES * frame_num / total_comp
    print(
        f"  Compression: {ratio:.2f}x  ({FRAME_BYTES * frame_num // 1024} KB -> {total_comp // 1024} KB)"
    )
    return frame_num


def main():
    parser = argparse.ArgumentParser(
        description="Convert video to compressed 1bpp e-ink frames (.mrv)"
    )
    parser.add_argument("video", help="Input video file")
    parser.add_argument(
        "--out", default="video", help="Output directory (default: ./video)"
    )
    parser.add_argument(
        "--fps", type=float, default=30.0, help="Target fps (default: 30)"
    )
    parser.add_argument("--start", default=None, help="Start time, e.g. 00:00:30")
    parser.add_argument("--end", default=None, help="End time, e.g. 00:01:00")
    parser.add_argument(
        "--white-point",
        type=int,
        default=220,
        metavar="N",
        help="Clamp grayscale >= N to white before dithering (default: 220)",
    )
    parser.add_argument(
        "--preview",
        type=int,
        default=0,
        metavar="N",
        help="Save every Nth frame as PNG to <out>/preview/",
    )
    args = parser.parse_args()

    if not os.path.isfile(args.video):
        sys.exit(f"File not found: {args.video}")

    clip = f" [{args.start}-{args.end}]" if (args.start or args.end) else ""
    print(f"Converting '{args.video}'{clip} -> {args.out}/video.mrv  at {args.fps} fps")

    n = video_to_mrv(
        args.video,
        args.out,
        args.fps,
        args.white_point,
        args.preview,
        args.start,
        args.end,
    )

    total_raw_mb = n * FRAME_BYTES / (1024 * 1024)
    print(
        f"\nDone: {n} frames, {total_raw_mb:.1f} MB uncompressed ({args.fps} fps = {n / args.fps:.1f}s)"
    )
    print(f"Copy '{args.out}/video.mrv' to the root of the SD card.")


if __name__ == "__main__":
    main()
