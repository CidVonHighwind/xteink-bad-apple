#!/usr/bin/env python3
"""
SSD1677 LUT Editor with Send-to-Device support.

Architecture:
- Each transition (B->B, B->W, W->B, W->W) has a voltage pattern that cycles.
- ALL transitions share the same timing groups (TP/RP).
- During Group N Phase A for X frames, each transition applies its own voltage.

Serial frame sent to device:
  [0xDE 0xAD 0xBE 0xEF]  magic (4 bytes)
  [length: uint32 LE]     payload length (4 bytes)
  [payload]               112-byte LUT
  [crc32: uint32 LE]      CRC-32/JAMCRC of payload (4 bytes)

The device logs the received bytes for verification; it does NOT yet apply the LUT.

Dependencies:
  pip install pyserial
"""

import binascii
import json
import struct
import threading
import tkinter as tk
from tkinter import messagebox, filedialog, scrolledtext, ttk
from typing import Dict, List, Tuple

try:
    import serial
    import serial.tools.list_ports

    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

VS_MAP = {0b00: "VSS", 0b01: "VSH1", 0b10: "VSL", 0b11: "VSH2"}
VS_REVERSE = {v: k for k, v in VS_MAP.items()}

TRANSITION_NAMES = ["Black → Black", "Black → White", "White → Black", "White → White"]
VOLTAGE_OPTIONS = ["VSS", "VSL", "VSH1", "VSH2"]

VOLTAGE_COLORS = {
    "VSS": "#757575",  # ground / off
    "VSL": "#2980b9",  # low / negative
    "VSH1": "#c0392b",  # high positive 1
    "VSH2": "#e67e22",  # high positive 2
}
VOLTAGE_DESCRIPTIONS = {
    "VSS": "Ground (0 V)",
    "VSL": "Low / negative",
    "VSH1": "High positive 1",
    "VSH2": "High positive 2",
}

FRAME_MAGIC = bytes([0xDE, 0xAD, 0xBE, 0xEF])
LUT_SIZE = 112


# ---------------------------------------------------------------------------
# LUT encoding helpers
# ---------------------------------------------------------------------------


def encode_lut(
    voltage_patterns: Dict[int, List[str]],
    timing_groups: List[List[int]],
    frame_rate: int,
    voltages: Dict[str, int],
) -> bytes:
    """Encode editor state into the 112-byte SSD1677 LUT."""
    lut = bytearray(LUT_SIZE)

    # VS blocks L0–L3 (10 bytes each, 4 steps per byte, 2 bits per step)
    for trans_idx in range(4):
        pattern = voltage_patterns[trans_idx]
        base = trans_idx * 10
        byte_idx = step_idx = 0
        while step_idx < len(pattern) and byte_idx < 10:
            byte_val = 0
            for phase in range(4):
                if step_idx < len(pattern):
                    shift = 6 - phase * 2
                    byte_val |= VS_REVERSE.get(pattern[step_idx], 0) << shift
                    step_idx += 1
            lut[base + byte_idx] = byte_val
            byte_idx += 1
    # L4 (VCOM) – stays 0x00

    # TP/RP groups: 10 groups × 5 bytes starting at offset 50
    for g in range(10):
        base = 50 + g * 5
        for i in range(5):
            lut[base + i] = timing_groups[g][i]

    # Frame rate (5 bytes, all the same)
    for i in range(5):
        lut[100 + i] = frame_rate

    # Voltages (VGH, VSH1, VSH2, VSL, VCOM)
    lut[105] = voltages["VGH"]
    lut[106] = voltages["VSH1"]
    lut[107] = voltages["VSH2"]
    lut[108] = voltages["VSL"]
    lut[109] = voltages["VCOM"]

    # Bytes 110–111: reserved, stay 0x00
    return bytes(lut)


def build_frame(payload: bytes) -> bytes:
    """Wrap a payload in the serial transfer frame."""
    length = struct.pack("<I", len(payload))
    crc = struct.pack("<I", binascii.crc32(payload) & 0xFFFFFFFF)
    return FRAME_MAGIC + length + payload + crc


# ---------------------------------------------------------------------------
# Main editor class
# ---------------------------------------------------------------------------


class LUTEditor:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("SSD1677 LUT Editor")
        self.root.geometry("1200x780")

        # --- LUT state ---
        self.voltage_patterns: Dict[int, List[str]] = {
            0: ["VSS", "VSH1", "VSS", "VSH1"],  # B->B
            1: ["VSL", "VSL", "VSL", "VSS"],  # B->W
            2: ["VSH1", "VSS", "VSH1", "VSS"],  # W->B
            3: ["VSS", "VSL", "VSS", "VSL"],  # W->W
        }
        self.timing_groups: List[List[int]] = [
            [4, 4, 0, 0, 0],
            [2, 2, 0, 0, 0],
            *[[0, 0, 0, 0, 0]] * 8,
        ]
        self.frame_rate = 0x88
        self.voltages = {
            "VGH": 0x17,
            "VSH1": 0x41,
            "VSH2": 0xA8,
            "VSL": 0x32,
            "VCOM": 0x30,
        }

        # UI widget holders
        self.voltage_widgets: Dict[int, dict] = {}
        self.timing_widgets: List[List[tk.Widget]] = []
        self.voltage_entries: Dict[str, tk.Entry] = {}

        self._build_ui()
        self.update_preview()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self):
        self.root.rowconfigure(0, weight=1)
        self.root.columnconfigure(0, weight=1)

        paned = tk.PanedWindow(
            self.root,
            orient=tk.HORIZONTAL,
            sashrelief="raised",
            sashwidth=5,
            bg="#cccccc",
        )
        paned.grid(row=0, column=0, sticky="nsew", padx=4, pady=4)

        left_outer = ttk.Frame(paned)
        right_outer = ttk.Frame(paned)
        paned.add(left_outer, stretch="always")
        paned.add(right_outer, stretch="always")

        self._build_left(left_outer)
        self._build_right(right_outer)

        # Set sash to 70% of window width after the window is fully drawn
        def _set_sash():
            paned.update_idletasks()
            paned.sash_place(0, int(paned.winfo_width() * 0.60), 0)

        self.root.after(50, _set_sash)

    def _build_left(self, parent: ttk.Frame):
        parent.rowconfigure(0, weight=1)
        parent.columnconfigure(0, weight=1)

        v_scroll = ttk.Scrollbar(parent, orient="vertical")
        v_scroll.grid(row=0, column=1, sticky="ns")

        canvas = tk.Canvas(parent, yscrollcommand=v_scroll.set, highlightthickness=0)
        canvas.grid(row=0, column=0, sticky="nsew")
        v_scroll.config(command=canvas.yview)

        inner = ttk.Frame(canvas)
        win_id = canvas.create_window((0, 0), window=inner, anchor="nw")

        # Keep inner frame as wide as the canvas
        canvas.bind("<Configure>", lambda e: canvas.itemconfig(win_id, width=e.width))
        inner.bind(
            "<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all")),
        )

        # Title
        hdr = ttk.Frame(inner)
        hdr.pack(fill=tk.X, padx=10, pady=(10, 4))
        ttk.Label(hdr, text="SSD1677 LUT Editor", font=("", 13, "bold")).pack()
        ttk.Label(
            hdr,
            text="Voltage patterns are per-transition · Timing groups are global",
            font=("", 9, "italic"),
        ).pack()

        # Voltage patterns
        vf = ttk.LabelFrame(inner, text="Voltage Patterns (per transition)", padding=8)
        vf.pack(fill=tk.X, padx=8, pady=4)
        self._build_voltage_legend(vf)
        self._build_voltage_section(vf)

        # Timing groups
        tf = ttk.LabelFrame(
            inner, text="Timing Groups (global – used by all transitions)", padding=8
        )
        tf.pack(fill=tk.X, padx=8, pady=4)
        for i in range(10):
            self._build_timing_row(tf, i)

        # Global settings
        sf = ttk.LabelFrame(inner, text="Global Settings", padding=8)
        sf.pack(fill=tk.X, padx=8, pady=(4, 10))
        self._build_settings(sf)

    # ---- voltage legend ----

    def _build_voltage_legend(self, parent):
        legend = ttk.Frame(parent)
        legend.pack(fill=tk.X, pady=(0, 6))
        ttk.Label(legend, text="Key: ", font=("Consolas", 8)).pack(side=tk.LEFT)
        for name in VOLTAGE_OPTIONS:
            color = VOLTAGE_COLORS[name]
            swatch = tk.Label(
                legend,
                text=f" {name} ",
                bg=color,
                fg="white",
                font=("Consolas", 8, "bold"),
                relief="flat",
                padx=4,
                pady=2,
            )
            swatch.pack(side=tk.LEFT, padx=3)
            ttk.Label(
                legend, text=VOLTAGE_DESCRIPTIONS[name], font=("Consolas", 8)
            ).pack(side=tk.LEFT)
            ttk.Label(legend, text="  ").pack(side=tk.LEFT)

    # ---- voltage pattern rows (shared scroll) ----

    def _build_voltage_section(self, parent):
        """One canvas + one scrollbar shared by all 4 transition rows."""
        body = ttk.Frame(parent)
        body.pack(fill=tk.X, expand=True)

        # Fixed left column: row labels
        labels_col = ttk.Frame(body)
        labels_col.pack(side=tk.LEFT)

        # Shared scrollable canvas
        self._vp_canvas = tk.Canvas(body, highlightthickness=0)
        self._vp_canvas.pack(side=tk.LEFT, fill=tk.X, expand=True)

        # Fixed right column: +/- buttons
        ctrl_col = ttk.Frame(body)
        ctrl_col.pack(side=tk.LEFT)

        # Shared scrollbar below the canvas
        h_scroll = ttk.Scrollbar(
            parent, orient="horizontal", command=self._vp_canvas.xview
        )
        h_scroll.pack(fill=tk.X)
        self._vp_canvas.configure(xscrollcommand=h_scroll.set)

        # Inner frame that holds all 4 button rows stacked vertically
        canvas_inner = ttk.Frame(self._vp_canvas)
        self._vp_canvas.create_window((0, 0), window=canvas_inner, anchor="nw")

        def _update_scrollregion(e):
            self._vp_canvas.configure(scrollregion=self._vp_canvas.bbox("all"))
            self._vp_canvas.configure(height=canvas_inner.winfo_reqheight())

        canvas_inner.bind("<Configure>", _update_scrollregion)

        for i in range(4):
            self._build_voltage_row(labels_col, canvas_inner, ctrl_col, i)

    # ---- voltage pattern row ----

    def _build_voltage_row(self, labels_col, canvas_inner, ctrl_col, trans_idx: int):
        ttk.Label(
            labels_col, text=f"{TRANSITION_NAMES[trans_idx]}:", width=18, anchor="w"
        ).pack(pady=3)

        container = ttk.Frame(canvas_inner)
        container.pack(anchor="nw", pady=3)

        ctrl = ttk.Frame(ctrl_col)
        ctrl.pack(pady=3)
        add_btn = ttk.Button(
            ctrl, text="+", width=3, command=lambda: self._add_voltage(trans_idx)
        )
        add_btn.pack(side=tk.LEFT, padx=2)
        rem_btn = ttk.Button(
            ctrl,
            text="−",
            width=3,
            command=lambda: self._remove_last_voltage(trans_idx),
        )
        rem_btn.pack(side=tk.LEFT, padx=2)

        self.voltage_widgets[trans_idx] = {
            "container": container,
            "add_btn": add_btn,
            "rem_btn": rem_btn,
            "btns": [],
        }
        self._reload_voltage_row(trans_idx)

    def _reload_voltage_row(self, trans_idx: int):
        widgets = self.voltage_widgets[trans_idx]
        container = widgets["container"]
        pattern = self.voltage_patterns[trans_idx]
        btns: list = widgets["btns"]

        # Grow: add missing buttons
        while len(btns) < len(pattern):
            idx = len(btns)
            btn = tk.Button(
                container,
                text="",
                fg="white",
                width=5,
                relief="raised",
                bd=2,
                font=("Consolas", 8, "bold"),
                cursor="hand2",
            )
            btn.pack(side=tk.LEFT, padx=2)
            btns.append(btn)

        # Shrink: remove excess buttons
        while len(btns) > len(pattern):
            btns.pop().destroy()

        # Update all buttons in-place (no flicker)
        for idx, btn in enumerate(btns):
            voltage = pattern[idx]
            btn.config(
                text=voltage,
                bg=VOLTAGE_COLORS[voltage],
                command=lambda t=trans_idx, v=idx: self._cycle_voltage(t, v),
            )

        widgets["add_btn"].config(state="disabled" if len(pattern) >= 40 else "normal")
        widgets["rem_btn"].config(state="disabled" if len(pattern) <= 1 else "normal")
        # Refresh the shared canvas scrollregion
        if hasattr(self, "_vp_canvas"):
            self._vp_canvas.after_idle(
                lambda: self._vp_canvas.configure(
                    scrollregion=self._vp_canvas.bbox("all")
                )
            )

    def _cycle_voltage(self, trans_idx: int, volt_idx: int):
        current = self.voltage_patterns[trans_idx][volt_idx]
        next_val = VOLTAGE_OPTIONS[
            (VOLTAGE_OPTIONS.index(current) + 1) % len(VOLTAGE_OPTIONS)
        ]
        self.voltage_patterns[trans_idx][volt_idx] = next_val
        # Update only the clicked button in-place — no flicker
        btn = self.voltage_widgets[trans_idx]["btns"][volt_idx]
        btn.config(text=next_val, bg=VOLTAGE_COLORS[next_val])
        self.update_preview()

    def _add_voltage(self, trans_idx: int):
        if len(self.voltage_patterns[trans_idx]) < 40:
            self.voltage_patterns[trans_idx].append("VSS")
            self._reload_voltage_row(trans_idx)
            self.update_preview()

    def _remove_last_voltage(self, trans_idx: int):
        if len(self.voltage_patterns[trans_idx]) > 1:
            self.voltage_patterns[trans_idx].pop()
            self._reload_voltage_row(trans_idx)
            self.update_preview()

    # ---- timing group row ----

    def _build_timing_row(self, parent, group_idx: int):
        row = ttk.Frame(parent)
        row.pack(fill=tk.X, pady=2)
        ttk.Label(row, text=f"Group {group_idx}:", width=10).pack(side=tk.LEFT)
        entries = []
        for pi, name in enumerate(["A", "B", "C", "D"]):
            ttk.Label(row, text=f"{name}:").pack(side=tk.LEFT, padx=2)
            var = tk.StringVar(value=str(self.timing_groups[group_idx][pi]))
            entry = ttk.Entry(row, textvariable=var, width=5)
            entry.pack(side=tk.LEFT, padx=2)
            entry.bind("<KeyRelease>", lambda e, g=group_idx: self._on_timing_change(g))
            entries.append(var)
        ttk.Label(row, text="RP:").pack(side=tk.LEFT, padx=5)
        var = tk.StringVar(value=str(self.timing_groups[group_idx][4]))
        entry = ttk.Entry(row, textvariable=var, width=5)
        entry.pack(side=tk.LEFT, padx=2)
        entry.bind("<KeyRelease>", lambda e, g=group_idx: self._on_timing_change(g))
        entries.append(var)
        self.timing_widgets.append(entries)

    def _on_timing_change(self, group_idx: int):
        for i, spin in enumerate(self.timing_widgets[group_idx]):
            try:
                self.timing_groups[group_idx][i] = int(spin.get() or 0)
            except ValueError:
                pass
        self.update_preview()

    # ---- global settings ----

    def _build_settings(self, parent):
        # Frame rate
        fr = ttk.Frame(parent)
        fr.pack(fill=tk.X, pady=5)
        ttk.Label(fr, text="Frame Rate:", font=("", 9, "bold")).pack(side=tk.LEFT)
        self.frame_rate_spin = ttk.Spinbox(fr, from_=1, to=255, width=6)
        self.frame_rate_spin.set(str(self.frame_rate))
        self.frame_rate_spin.pack(side=tk.LEFT, padx=5)
        for event in ("<KeyRelease>", "<<Increment>>", "<<Decrement>>"):
            self.frame_rate_spin.bind(event, lambda e: self._on_framerate_change())
        ttk.Label(fr, text="(lower = slower/more stable)").pack(side=tk.LEFT)

        # Voltages
        vf = ttk.Frame(parent)
        vf.pack(fill=tk.X, pady=5)
        ttk.Label(vf, text="Voltages:", font=("", 9, "bold")).pack(anchor=tk.W)
        hrow = ttk.Frame(vf)
        hrow.pack(fill=tk.X, pady=2)
        for name in ["VGH", "VSH1", "VSH2", "VSL", "VCOM"]:
            ttk.Label(hrow, text=f"{name}:").pack(side=tk.LEFT, padx=(6, 1))
            entry = ttk.Entry(hrow, width=7)
            entry.insert(0, f"0x{self.voltages[name]:02X}")
            entry.pack(side=tk.LEFT)
            entry.bind("<KeyRelease>", lambda e: self._on_voltage_setting_change())
            self.voltage_entries[name] = entry

    def _on_framerate_change(self):
        try:
            self.frame_rate = int(self.frame_rate_spin.get() or 0x44)
        except ValueError:
            pass
        self.update_preview()

    def _on_voltage_setting_change(self):
        for name, entry in self.voltage_entries.items():
            try:
                text = entry.get().strip()
                self.voltages[name] = (
                    int(text, 16 if text.startswith(("0x", "0X")) else 10) & 0xFF
                )
            except ValueError:
                pass
        self.update_preview()

    # ------------------------------------------------------------------
    # Right panel
    # ------------------------------------------------------------------

    def _build_right(self, parent: ttk.Frame):
        parent.columnconfigure(0, weight=1)
        # rows that should expand:  C-array (row 2) and device log (row 5)
        parent.rowconfigure(2, weight=2)
        parent.rowconfigure(5, weight=3)

        # row 0 – action buttons
        btn_row = ttk.Frame(parent)
        btn_row.grid(row=0, column=0, sticky="ew", padx=4, pady=(4, 2))
        for text, cmd in [
            ("Load", self._load),
            ("Save", self._save),
            ("Reset", self._reset),
            ("Copy", self._copy),
        ]:
            ttk.Button(btn_row, text=text, command=cmd).pack(side=tk.LEFT, padx=2)

        # row 1 – timing info
        info = ttk.LabelFrame(parent, text="Timing Info", padding=4)
        info.grid(row=1, column=0, sticky="ew", padx=4, pady=2)
        self.timing_label = ttk.Label(info, text="", font=("", 9))
        self.timing_label.pack()

        # row 2 – C array (expands)
        preview = ttk.LabelFrame(parent, text="C Array Output", padding=4)
        preview.grid(row=2, column=0, sticky="nsew", padx=4, pady=2)
        preview.rowconfigure(0, weight=1)
        preview.columnconfigure(0, weight=1)
        self.preview_text = scrolledtext.ScrolledText(
            preview, wrap=tk.WORD, font=("Consolas", 9)
        )
        self.preview_text.grid(row=0, column=0, sticky="nsew")

        # row 3 – send to device
        send_frame = ttk.LabelFrame(parent, text="Send to Device", padding=6)
        send_frame.grid(row=3, column=0, sticky="ew", padx=4, pady=2)

        ctrl_row = ttk.Frame(send_frame)
        ctrl_row.pack(fill=tk.X)
        ttk.Label(ctrl_row, text="Port:").pack(side=tk.LEFT)
        self.port_var = tk.StringVar(value="COM4")
        self.port_combo = ttk.Combobox(ctrl_row, textvariable=self.port_var, width=9)
        self.port_combo.pack(side=tk.LEFT, padx=(2, 0))
        ttk.Button(ctrl_row, text="↻", width=2, command=self._refresh_ports).pack(
            side=tk.LEFT, padx=(1, 8)
        )
        ttk.Label(ctrl_row, text="Baud:").pack(side=tk.LEFT)
        self.baud_var = tk.StringVar(value="115200")
        ttk.Combobox(
            ctrl_row,
            textvariable=self.baud_var,
            values=["115200", "230400", "460800", "921600"],
            width=8,
            state="readonly",
        ).pack(side=tk.LEFT, padx=(2, 8))
        ttk.Button(ctrl_row, text="▶  Send LUT", command=self._send_to_device).pack(
            side=tk.LEFT, padx=2
        )
        self._refresh_ports()

        if not SERIAL_AVAILABLE:
            ttk.Label(
                send_frame,
                text="⚠  pyserial not installed  (pip install pyserial)",
                foreground="red",
            ).pack(anchor="w")

        # row 4 – log header
        log_header = ttk.Frame(parent)
        log_header.grid(row=4, column=0, sticky="ew", padx=4, pady=(6, 0))
        ttk.Label(log_header, text="Device Log", font=("", 9, "bold")).pack(
            side=tk.LEFT
        )
        ttk.Button(log_header, text="Clear", command=self._clear_log).pack(
            side=tk.RIGHT
        )

        # row 5 – device log (expands)
        self.log_text = scrolledtext.ScrolledText(
            parent, wrap=tk.WORD, font=("Consolas", 9), state=tk.DISABLED
        )
        self.log_text.grid(row=5, column=0, sticky="nsew", padx=4, pady=(0, 4))
        self.log_text.tag_config("ok", foreground="#00aa00")
        self.log_text.tag_config("err", foreground="#cc0000")
        self.log_text.tag_config("warn", foreground="#cc7700")
        self.log_text.tag_config("info", foreground="#222222")

    # ------------------------------------------------------------------
    # Preview / encode
    # ------------------------------------------------------------------

    def _calc_timing(self) -> Tuple[int, int]:
        total_frames = sum(sum(g[:4]) * (g[4] + 1) for g in self.timing_groups)
        ms = max(10.0, 2500.0 / self.frame_rate if self.frame_rate else 50.0)
        return total_frames, int(total_frames * ms * 1.1)

    def update_preview(self):
        lut = encode_lut(
            self.voltage_patterns, self.timing_groups, self.frame_rate, self.voltages
        )
        frames, ms = self._calc_timing()
        self.timing_label.config(text=f"Total frames: {frames} | Est. time: ~{ms} ms")

        out = "const uint8_t lut_custom[] = {\n"
        out += "  // VS L0–L3 (voltage patterns per transition)\n"
        for t in range(4):
            out += f"  // {TRANSITION_NAMES[t]}: [{' → '.join(self.voltage_patterns[t])}]\n  "
            out += "".join(f"0x{lut[t * 10 + b]:02X}," for b in range(10)) + "\n"
        out += "  // L4 (VCOM)\n  "
        out += "".join(f"0x{lut[40 + i]:02X}," for i in range(10)) + "\n\n"
        out += "  // TP/RP groups\n"
        for g in range(10):
            base = 50 + g * 5
            t = self.timing_groups[g]
            out += "  " + "".join(f"0x{lut[base + i]:02X}," for i in range(5))
            out += f"  // G{g}: A={t[0]} B={t[1]} C={t[2]} D={t[3]} RP={t[4]}"
            total = sum(t[:4]) * (t[4] + 1)
            if total:
                out += f" ({total} frames)"
            out += "\n"
        out += "\n  // Frame rate\n  "
        out += "".join(f"0x{lut[100 + i]:02X}," for i in range(5)) + "\n"
        out += "\n  // Voltages (VGH, VSH1, VSH2, VSL, VCOM)\n  "
        out += "".join(f"0x{lut[105 + i]:02X}," for i in range(5)) + "\n"
        out += "\n  // Reserved\n  0x00, 0x00\n};\n"

        self.preview_text.delete("1.0", tk.END)
        self.preview_text.insert("1.0", out)

    # ------------------------------------------------------------------
    # Serial send
    # ------------------------------------------------------------------

    def _refresh_ports(self):
        if not SERIAL_AVAILABLE:
            return
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo["values"] = ports
        if self.port_var.get() not in ports and ports:
            self.port_var.set(ports[0])

    def _log(self, text: str, tag: str = "info"):
        """Append a line to the device log (thread-safe via after)."""

        def _append():
            self.log_text.config(state=tk.NORMAL)
            self.log_text.insert(tk.END, text + "\n", tag)
            self.log_text.see(tk.END)
            self.log_text.config(state=tk.DISABLED)

        self.root.after(0, _append)

    def _clear_log(self):
        self.log_text.config(state=tk.NORMAL)
        self.log_text.delete("1.0", tk.END)
        self.log_text.config(state=tk.DISABLED)

    def _send_to_device(self):
        if not SERIAL_AVAILABLE:
            messagebox.showerror(
                "Missing dependency",
                "pyserial is not installed.\nRun: pip install pyserial",
            )
            return
        port = self.port_var.get().strip()
        if not port:
            messagebox.showerror("No port", "Select a COM port first.")
            return
        lut = encode_lut(
            self.voltage_patterns, self.timing_groups, self.frame_rate, self.voltages
        )
        frame = build_frame(lut)
        baud = int(self.baud_var.get())
        threading.Thread(
            target=self._do_send, args=(port, baud, frame), daemon=True
        ).start()

    def _do_send(self, port: str, baud: int, frame: bytes):
        """Background thread: open port, write frame, read responses for 4 s."""
        self._log(f"─── Connecting to {port} @ {baud} ───", "info")
        try:
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = baud
            ser.timeout = 0.2
            ser.dtr = False
            ser.rts = False
            ser.open()
            try:
                ser.reset_input_buffer()
                ser.write(frame)
                self._log(
                    f"→ Sent {len(frame)} bytes  "
                    f"(magic+len+{len(frame)-12}B payload+crc)",
                    "ok",
                )
                # Read device responses for 4 seconds
                import time

                deadline = time.time() + 4.0
                buf = b""
                while time.time() < deadline:
                    chunk = ser.read(256)
                    if chunk:
                        buf += chunk
                        # Flush complete lines to the log
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            text = line.decode("utf-8", errors="replace").rstrip("\r")
                            tag = (
                                "ok"
                                if "OK" in text
                                else (
                                    "err"
                                    if "ERR" in text or "mismatch" in text.lower()
                                    else "warn" if "WARN" in text else "info"
                                )
                            )
                            self._log(text, tag)
                if buf.strip():
                    self._log(buf.decode("utf-8", errors="replace").rstrip(), "info")
                self._log("─── Done ───", "info")
            finally:
                ser.close()
        except serial.SerialException as exc:
            self._log(f"✗ Serial error: {exc}", "err")
        except Exception as exc:
            self._log(f"✗ Unexpected error: {exc}", "err")

    # ------------------------------------------------------------------
    # File I/O
    # ------------------------------------------------------------------

    def _save(self):
        path = filedialog.asksaveasfilename(
            defaultextension=".json",
            filetypes=[("JSON", "*.json"), ("All files", "*.*")],
        )
        if path:
            with open(path, "w") as f:
                json.dump(
                    {
                        "voltage_patterns": self.voltage_patterns,
                        "timing_groups": self.timing_groups,
                        "frame_rate": self.frame_rate,
                        "voltages": self.voltages,
                    },
                    f,
                    indent=2,
                )
            messagebox.showinfo("Saved", f"Saved to {path}")

    def _load(self):
        path = filedialog.askopenfilename(
            filetypes=[("JSON", "*.json"), ("All files", "*.*")]
        )
        if not path:
            return
        try:
            with open(path) as f:
                data = json.load(f)
            self.voltage_patterns = {
                int(k): v for k, v in data["voltage_patterns"].items()
            }
            self.timing_groups = data["timing_groups"]
            self.frame_rate = data["frame_rate"]
            self.voltages = data["voltages"]

            for i in range(4):
                self._reload_voltage_row(i)
            for g in range(10):
                for t in range(5):
                    self.timing_widgets[g][t].set(str(self.timing_groups[g][t]))
            self.frame_rate_spin.set(str(self.frame_rate))
            for name in self.voltages:
                self.voltage_entries[name].delete(0, tk.END)
                self.voltage_entries[name].insert(0, f"0x{self.voltages[name]:02X}")
            self.update_preview()
            messagebox.showinfo("Loaded", f"Loaded from {path}")
        except Exception as exc:
            messagebox.showerror("Load failed", str(exc))

    def _reset(self):
        if not messagebox.askyesno("Reset", "Reset to default LUT?"):
            return
        self.voltage_patterns = {
            0: ["VSS", "VSH1", "VSS", "VSH1"],
            1: ["VSL", "VSL", "VSL", "VSS"],
            2: ["VSH1", "VSS", "VSH1", "VSS"],
            3: ["VSS", "VSL", "VSS", "VSL"],
        }
        self.timing_groups = [[4, 4, 0, 0, 0], [2, 2, 0, 0, 0], *[[0, 0, 0, 0, 0]] * 8]
        self.frame_rate = 0x88
        self.voltages = {
            "VGH": 0x17,
            "VSH1": 0x41,
            "VSH2": 0xA8,
            "VSL": 0x32,
            "VCOM": 0x30,
        }
        for i in range(4):
            self._reload_voltage_row(i)
        for g in range(10):
            for t in range(5):
                self.timing_widgets[g][t].set(str(self.timing_groups[g][t]))
        self.frame_rate_spin.set(str(self.frame_rate))
        for name in self.voltages:
            self.voltage_entries[name].delete(0, tk.END)
            self.voltage_entries[name].insert(0, f"0x{self.voltages[name]:02X}")
        self.update_preview()

    def _copy(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.preview_text.get("1.0", tk.END))
        messagebox.showinfo("Copied", "C array copied to clipboard.")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    root = tk.Tk()
    LUTEditor(root)
    root.mainloop()
