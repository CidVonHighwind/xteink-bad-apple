# Xteink X4 Bad Apple

> [!WARNING]
> This project uses a custom waveform LUT that drives the e-ink panel outside of its normal operating parameters. It may cause permanent damage to your display. Use at your own risk — I take no responsibility for any damage caused.

Plays Bad Apple on an 800×480 e-ink display driven by an ESP32-C3.

Uses a custom fast-refresh LUT for the SSD1677 controller to achieve smooth playback at ~30 fps. Frames are read from an SD card and overlapped with display refresh to maximise throughput.

## SD card

Place a `video.mrv` file in the root of the SD card. Generate it from any video with the included tool:

```bash
pip install -r tools/requirements.txt
python tools/render_frames.py <input_video> --out <output_dir>
```

The `.mrv` format is PackBits-compressed 1bpp frames at 800×480 with an offset table for random access.

## Building & flashing

Requires [PlatformIO](https://platformio.org/).

```bash
pio run --target upload
```

Serial monitor at 115200 baud:

```bash
pio device monitor
```

## Controls

| Button | Action |
|---|---|
| Button0 | Play / pause |
| Button1 | Restart |
| Button2 | Switch demo (video ↔ balls) |
| Up | Step forward (while paused) |
| Down | Step backward (while paused) |

## LUT editor

A Tkinter GUI for tuning the SSD1677 waveform LUT in real time over USB serial:

```bash
python tools/lut_editor.py
```
