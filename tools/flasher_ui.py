"""RA Offline Programmer - desktop UI (Renesas RA2E1 over SWD).

Talks to the ESP8266 programmer over UART. Loads an Intel HEX / Motorola S-record
file from e2 studio, uploads it to the programmer's flash, configures the
programmer, and shows live pass/fail counters.

Requires: Python 3.8+, pyserial  (pip install pyserial)
"""

import csv
import datetime as dt
import json
import os
import queue
import threading
import time
import tkinter as tk
import zlib
from tkinter import filedialog, messagebox, ttk

import serial
import serial.tools.list_ports

APP_TITLE = "RA Offline Programmer"
DEFAULT_BAUD = 460800
CHUNK = 256
LOG_CSV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "flash_log.csv")

SETTING_KEYS = ("mode", "erase", "verify", "run", "reset", "swdelay", "settle")
SETTING_DEFAULTS = {"mode": "button", "erase": "chip", "verify": "1", "run": "1", "reset": "1", "swdelay": "4",
                    "settle": "500"}

# RA2E1 memory map (R7FA2E1A9 / A7 / A5: 128 / 64 / 32 KB code flash)
CF_BASE, CF_MAX = 0x00000000, 128 * 1024
DF_BASE, DF_SIZE = 0x40100000, 4 * 1024
EXTRA_BASE, EXTRA_END = 0x01010000, 0x01010040  # option-setting extra area: ID code (OSIS), access window


# --------------------------------------------------------------------------- image loading

def parse_intel_hex(text):
    """Return {address: byte} for an Intel HEX file."""
    mem = {}
    upper = 0
    for ln, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ValueError(f"line {ln}: missing ':'")
        try:
            rec = bytes.fromhex(line[1:])
        except ValueError:
            raise ValueError(f"line {ln}: invalid hex characters")
        if len(rec) < 5 or len(rec) != rec[0] + 5:
            raise ValueError(f"line {ln}: bad record length")
        if sum(rec) & 0xFF:
            raise ValueError(f"line {ln}: checksum error")
        addr, rtype, data = (rec[1] << 8) | rec[2], rec[3], rec[4:4 + rec[0]]
        if rtype == 0x00:
            base = upper + addr
            for i, b in enumerate(data):
                mem[base + i] = b
        elif rtype == 0x01:
            break
        elif rtype == 0x02:
            upper = ((data[0] << 8) | data[1]) << 4
        elif rtype == 0x04:
            upper = ((data[0] << 8) | data[1]) << 16
        # 0x03 / 0x05 (start address) are irrelevant for programming
    return mem


def parse_srec(text):
    """Return {address: byte} for a Motorola S-record file (e2 studio .srec / .mot)."""
    mem = {}
    addr_len = {"1": 2, "2": 3, "3": 4}
    for ln, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if line[0] != "S" or len(line) < 4:
            raise ValueError(f"line {ln}: not an S-record")
        kind = line[1]
        try:
            rec = bytes.fromhex(line[2:])
        except ValueError:
            raise ValueError(f"line {ln}: invalid hex characters")
        if len(rec) != rec[0] + 1:
            raise ValueError(f"line {ln}: bad record length")
        if (sum(rec) & 0xFF) != 0xFF:
            raise ValueError(f"line {ln}: checksum error")
        if kind in addr_len:
            n = addr_len[kind]
            base = int.from_bytes(rec[1:1 + n], "big")
            for i, b in enumerate(rec[1 + n:-1]):
                mem[base + i] = b
        elif kind in "789":
            break
        # S0 header, S5/S6 counts: nothing to program
    return mem


def load_image(path):
    """Load .hex / .srec / .bin. Returns a dict with the code flash and data flash blobs."""
    with open(path, "rb") as f:
        raw = f.read()
    low = path.lower()
    if low.endswith((".bin",)):
        mem = {CF_BASE + i: b for i, b in enumerate(raw)}
    elif raw.lstrip()[:1] == b":":
        mem = parse_intel_hex(raw.decode("ascii", errors="replace"))
    elif raw.lstrip()[:1] == b"S":
        mem = parse_srec(raw.decode("ascii", errors="replace"))
    else:
        raise ValueError("not an Intel HEX, S-record or .bin file")
    if not mem:
        raise ValueError("no data records in file")
    return build_image(mem)


def build_image(mem):
    cf, df, extra, other = {}, {}, {}, []
    for a, b in mem.items():
        if CF_BASE <= a < CF_BASE + CF_MAX:
            cf[a - CF_BASE] = b
        elif DF_BASE <= a < DF_BASE + DF_SIZE:
            df[a - DF_BASE] = b
        elif EXTRA_BASE <= a < EXTRA_END:
            extra[a] = b
        else:
            other.append(a)
    if other:
        raise ValueError(f"data at 0x{min(other):08X}-0x{max(other):08X} is outside the RA2E1 code flash "
                         f"(0x00000000), data flash (0x40100000) and option area")
    custom = sorted(a for a, b in extra.items() if b != 0xFF)
    if custom:
        raise ValueError(f"the file sets the ID code / access window at 0x{custom[0]:08X} (option-setting extra "
                         f"area). This programmer only supports the default (all 0xFF, unlocked) setting.")

    def blob(m, align):
        if not m:
            return b""
        size = max(m) + 1
        size += -size % align
        buf = bytearray(b"\xff" * size)
        for off, b in m.items():
            buf[off] = b
        return bytes(buf)

    cf_blob, df_blob = blob(cf, 4), blob(df, 1)
    return {"cf": cf_blob, "df": df_blob, "data": cf_blob + df_blob, "cf_used": len(cf), "df_used": len(df),
            "extra": len(extra)}


def describe_image(im):
    parts = [f"Code flash {len(im['cf']):,} B (0x00000000-0x{len(im['cf']) - 1:08X})" if im["cf"] else
             "No code flash data"]
    if im["df"]:
        parts.append(f"Data flash {len(im['df']):,} B")
    if im["extra"]:
        parts.append("ID code unlocked")
    return "   ·   ".join(parts)


# --------------------------------------------------------------------------- serial link

class LinkError(Exception):
    pass


class SerialLink:
    """Line-based link. '@OK'/'@ERR' lines answer requests, everything else is an event."""

    def __init__(self, on_line):
        self.ser = None
        self.on_line = on_line
        self.resp_q = queue.Queue()
        self.lock = threading.Lock()
        self.alive = False

    @property
    def is_open(self):
        return self.ser is not None and self.ser.is_open

    def open(self, port, baud):
        ser = serial.Serial()
        ser.port, ser.baudrate, ser.timeout = port, baud, 0.05
        # NodeMCU wires DTR/RTS to GPIO0/EN; keep them released so the ESP is not reset.
        ser.dtr = False
        ser.rts = False
        ser.open()
        self.ser = ser
        self.alive = True
        threading.Thread(target=self._reader, daemon=True).start()

    def close(self):
        self.alive = False
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def _reader(self):
        buf = b""
        while self.alive:
            try:
                chunk = self.ser.read(512)
            except Exception as e:
                self.on_line(f"@EVT LINKLOST {e}")
                self.alive = False
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", errors="replace").strip()
                if not line:
                    continue
                if line.startswith("@OK") or line.startswith("@ERR"):
                    self.resp_q.put(line)
                else:
                    self.on_line(line)

    def request(self, cmd, timeout=3.0):
        """Send a command, return the reply payload (text after '@OK <CMD>'), raise LinkError on @ERR."""
        if not self.is_open:
            raise LinkError("not connected")
        name = cmd.split(" ", 1)[0].upper()
        with self.lock:
            while not self.resp_q.empty():
                self.resp_q.get_nowait()
            self.ser.write((cmd + "\n").encode("ascii"))
            deadline = time.time() + timeout
            while True:
                left = deadline - time.time()
                if left <= 0:
                    raise LinkError(f"{name}: no reply from programmer")
                try:
                    line = self.resp_q.get(timeout=left)
                except queue.Empty:
                    continue
                parts = line.split(" ", 2)
                if len(parts) < 2 or parts[1] != name:
                    continue
                payload = parts[2] if len(parts) > 2 else ""
                if parts[0] == "@ERR":
                    raise LinkError(f"{name}: {payload}")
                return payload


def is_bluetooth_port(port):
    """Windows creates virtual COM ports for paired Bluetooth devices; they are never the programmer."""
    text = f"{port.hwid} {port.description}".upper()
    return "BTHENUM" in text or "BLUETOOTH" in text


def parse_kv(text):
    out = {}
    for tok in text.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


# --------------------------------------------------------------------------- UI

PREFS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui_prefs.json")

C = {
    # dark theme (slate)
    "bg": "#0e131b", "card": "#161d28", "border": "#273244", "text": "#e6eaf0", "muted": "#8c99ad",
    "accent": "#3b82f6", "accent_hover": "#2563eb", "accent_glow": "#60a5fa", "accent_soft": "#172a46",
    "accent_text": "#93c5fd",
    "ok": "#22c55e", "ok_hover": "#16a34a", "ok_soft": "#0f2a1d", "ok_text": "#4ade80",
    "err": "#ef4444", "err_hover": "#dc2626", "err_soft": "#341719", "err_text": "#f87171",
    "warn": "#fbbf24", "warn_soft": "#33280e", "idle_soft": "#1c2433",
    "header": "#090d13", "header_text": "#e6eaf0", "header_muted": "#7c899c", "disabled": "#4b5567",
    "btn": "#263142", "btn_hover": "#303d52", "btn_pressed": "#3a4962", "btn_border": "#3d4c64",
    "btn_dis": "#1b2230", "btn_dis_border": "#262f3f", "btn_dis_text": "#5d6a7d", "acc_dis": "#2a3446",
    "field": "#0f1520", "field_border": "#334155", "log_bg": "#0f1520", "log_time": "#5d6a7d",
    "pill_on_bg": "#0f2a1d", "pill_on_fg": "#4ade80", "pill_off_bg": "#341719", "pill_off_fg": "#fca5a5",
}
FONT = "Segoe UI"
MONO = "Consolas"


def load_prefs():
    try:
        with open(PREFS_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_prefs(prefs):
    try:
        with open(PREFS_FILE, "w", encoding="utf-8") as f:
            json.dump(prefs, f, indent=2)
    except OSError:
        pass


class App(tk.Tk):
    STATE_TEXT = {"connect": "Connecting to target", "erase": "Erasing", "program": "Programming",
                  "verify": "Verifying"}

    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        screen_h = self.winfo_screenheight()
        self.geometry(f"1200x{min(960, screen_h - 80)}")
        self.minsize(1060, 760)
        if screen_h < 1000:
            self.state("zoomed")  # small laptop screens: use the whole screen
        self.configure(bg=C["bg"])
        self.ui_q = queue.Queue()
        self.link = SerialLink(self.ui_q.put)
        self.prefs = load_prefs()
        self.image = None          # (path, image dict, crc)
        self.dev_info = {}
        self.busy = False          # a PC-side request is running
        self.device_busy = False   # the programmer is flashing
        self.suppress_set = False  # don't echo settings back while loading them
        self.conn_widgets = []     # enabled only while connected
        self.pulse_on = False

        self._styles()
        self._build()
        self.refresh_ports()
        self._set_connected(False)
        last = self.prefs.get("last_file")
        if last and os.path.exists(last):
            self._load_file(last, quiet=True)
        self.bind("<F5>", lambda e: self.flash_now())
        self.bind("<Control-o>", lambda e: self.browse())
        self.after(50, self._drain)
        self.after(700, self._pulse)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ================================================================ styling
    def _styles(self):
        s = ttk.Style(self)
        s.theme_use("clam")
        base = (FONT, 10)
        s.configure(".", font=base, background=C["bg"], foreground=C["text"], bordercolor=C["border"],
                    lightcolor=C["border"], darkcolor=C["border"], troughcolor=C["idle_soft"],
                    focuscolor=C["accent"], selectbackground=C["accent"], selectforeground="#ffffff",
                    fieldbackground=C["field"], insertcolor=C["text"], arrowcolor=C["text"])
        s.configure("TFrame", background=C["bg"])
        s.configure("Card.TFrame", background=C["card"])
        s.configure("Header.TFrame", background=C["header"])
        s.configure("TLabel", background=C["bg"], foreground=C["text"])
        s.configure("Card.TLabel", background=C["card"])
        s.configure("Muted.TLabel", background=C["card"], foreground=C["muted"], font=(FONT, 9))
        s.configure("CardTitle.TLabel", background=C["card"], font=(FONT, 11, "bold"))
        s.configure("Step.TLabel", background=C["accent_soft"], foreground=C["accent_text"],
                    font=(FONT, 9, "bold"), padding=(7, 1))
        s.configure("FileName.TLabel", background=C["card"], font=(FONT, 15, "bold"))
        s.configure("Header.TLabel", background=C["header"], foreground=C["header_text"])
        s.configure("HeaderTitle.TLabel", background=C["header"], foreground="#ffffff", font=(FONT, 15, "bold"))
        s.configure("HeaderSub.TLabel", background=C["header"], foreground=C["header_muted"], font=(FONT, 9))

        def button(style, bg, hover, pressed, border, fg, dis_bg, dis_fg, dis_border, **extra):
            s.configure(style, background=bg, foreground=fg, bordercolor=border, lightcolor=bg, darkcolor=bg,
                        relief="flat", **extra)
            fills = [("disabled", dis_bg), ("pressed", pressed), ("active", hover)]
            s.map(style, background=fills, lightcolor=fills, darkcolor=fills,
                  bordercolor=[("disabled", dis_border), ("active", hover if border == bg else C["accent"])],
                  foreground=[("disabled", dis_fg)])

        # Secondary buttons: slate fill with a visible border.
        button("TButton", C["btn"], C["btn_hover"], C["btn_pressed"], C["btn_border"], C["text"],
               C["btn_dis"], C["btn_dis_text"], C["btn_dis_border"], padding=(12, 6), font=(FONT, 10, "bold"))
        for name, bg, hover in (("Accent", C["accent"], C["accent_hover"]),
                                ("Pulse", C["accent_glow"], C["accent"]),
                                ("Ok", C["ok"], C["ok_hover"]),
                                ("Danger", C["err"], C["err_hover"])):
            button(f"{name}.TButton", bg, hover, hover, bg, "#ffffff", C["acc_dis"], C["btn_dis_text"],
                   C["acc_dis"], padding=(16, 7), font=(FONT, 10, "bold"))
        button("Flash.TButton", C["accent"], C["accent_hover"], C["accent_hover"], C["accent"], "#ffffff",
               C["acc_dis"], C["btn_dis_text"], C["acc_dis"], padding=(20, 14), font=(FONT, 16, "bold"))
        button("Header.TButton", C["btn"], C["btn_hover"], C["btn_pressed"], C["btn_border"], "#ffffff",
               C["header"], C["btn_dis_text"], C["btn_dis_border"], padding=(6, 1), font=(FONT, 12, "bold"))
        s.configure("Link.TButton", background=C["card"], foreground=C["accent_text"], padding=(4, 2),
                    bordercolor=C["card"], lightcolor=C["card"], darkcolor=C["card"], relief="flat",
                    font=(FONT, 9, "underline"))
        s.map("Link.TButton", background=[("active", C["accent_soft"])],
              lightcolor=[("active", C["accent_soft"])], darkcolor=[("active", C["accent_soft"])],
              bordercolor=[("active", C["accent_soft"])], foreground=[("disabled", C["btn_dis_text"])])

        for style in ("Card.TCheckbutton", "Card.TRadiobutton"):
            s.configure(style, background=C["card"], foreground=C["text"], indicatorbackground=C["field"],
                        indicatorforeground=C["accent_text"], upperbordercolor=C["field_border"],
                        lowerbordercolor=C["field_border"])
            s.map(style, background=[("active", C["card"])],
                  indicatorbackground=[("disabled", C["btn_dis"]), ("pressed", C["btn"])],
                  foreground=[("disabled", C["btn_dis_text"])])
        s.configure("Seg.Toolbutton", background=C["btn"], foreground=C["text"], padding=(14, 5),
                    font=(FONT, 9, "bold"), anchor="center", bordercolor=C["btn_border"], relief="flat",
                    lightcolor=C["btn"], darkcolor=C["btn"])
        seg = [("disabled", C["btn_dis"]), ("selected", C["accent"]), ("active", C["btn_hover"])]
        s.map("Seg.Toolbutton", background=seg, lightcolor=seg, darkcolor=seg,
              bordercolor=[("disabled", C["btn_dis_border"]), ("selected", C["accent"])],
              foreground=[("disabled", C["btn_dis_text"]), ("selected", "#ffffff")])

        for style in ("TCombobox", "Header.TCombobox"):
            s.configure(style, padding=4, arrowsize=14, fieldbackground=C["field"], background=C["btn"],
                        foreground=C["text"], arrowcolor=C["text"], bordercolor=C["field_border"],
                        lightcolor=C["field"], darkcolor=C["field"])
            s.map(style, fieldbackground=[("readonly", C["field"]), ("disabled", C["btn_dis"])],
                  background=[("active", C["btn_hover"]), ("disabled", C["btn_dis"])],
                  foreground=[("disabled", C["btn_dis_text"]), ("readonly", C["text"])],
                  arrowcolor=[("disabled", C["btn_dis_text"])],
                  selectbackground=[("readonly", C["field"])], selectforeground=[("readonly", C["text"])])
        self.option_add("*TCombobox*Listbox.font", base)
        self.option_add("*TCombobox*Listbox.background", C["field"])
        self.option_add("*TCombobox*Listbox.foreground", C["text"])
        self.option_add("*TCombobox*Listbox.selectBackground", C["accent"])
        self.option_add("*TCombobox*Listbox.selectForeground", "#ffffff")
        for style in ("TEntry", "TSpinbox"):
            s.configure(style, fieldbackground=C["field"], foreground=C["text"], bordercolor=C["field_border"],
                        lightcolor=C["field"], darkcolor=C["field"], insertcolor=C["text"], padding=3,
                        arrowsize=12, background=C["btn"], arrowcolor=C["text"])
            s.map(style, fieldbackground=[("disabled", C["btn_dis"])], foreground=[("disabled", C["btn_dis_text"])],
                  bordercolor=[("focus", C["accent"])], arrowcolor=[("disabled", C["btn_dis_text"])])
        s.configure("Vertical.TScrollbar", background=C["btn"], troughcolor=C["log_bg"], bordercolor=C["log_bg"],
                    lightcolor=C["btn"], darkcolor=C["btn"], arrowcolor=C["muted"])
        s.map("Vertical.TScrollbar", background=[("active", C["btn_hover"])])
        s.configure("Horizontal.TProgressbar", thickness=10, background=C["accent"], troughcolor=C["idle_soft"],
                    bordercolor=C["idle_soft"], lightcolor=C["accent"], darkcolor=C["accent"])
        s.configure("Ok.Horizontal.TProgressbar", background=C["ok"], lightcolor=C["ok"], darkcolor=C["ok"])
        s.configure("Err.Horizontal.TProgressbar", background=C["err"], lightcolor=C["err"], darkcolor=C["err"])

    def _card(self, parent, title, step=None, action=None):
        """Card with a title row. Returns the outer frame; its body is .body."""
        outer = tk.Frame(parent, bg=C["card"], highlightthickness=1, highlightbackground=C["border"])
        head = ttk.Frame(outer, style="Card.TFrame", padding=(14, 10, 14, 0))
        head.pack(fill="x")
        if step:
            ttk.Label(head, text=step, style="Step.TLabel").pack(side="left", padx=(0, 8))
        ttk.Label(head, text=title, style="CardTitle.TLabel").pack(side="left")
        if action:
            txt, fn = action
            b = ttk.Button(head, text=txt, style="Link.TButton", command=fn)
            b.pack(side="right")
            self.conn_widgets.append(b)
        body = ttk.Frame(outer, style="Card.TFrame", padding=(14, 8, 14, 12))
        body.pack(fill="both", expand=True)
        outer.body = body
        return outer

    def _track(self, *widgets):
        self.conn_widgets.extend(widgets)
        return widgets[0]

    # ================================================================ layout
    def _build(self):
        # ---- header
        header = ttk.Frame(self, style="Header.TFrame", padding=(18, 12))
        header.pack(fill="x")
        title = ttk.Frame(header, style="Header.TFrame")
        title.pack(side="left")
        ttk.Label(title, text="RA Offline Programmer", style="HeaderTitle.TLabel").pack(anchor="w")
        ttk.Label(title, text="Renesas RA2E1  ·  SWD production flasher  ·  ESP8266",
                  style="HeaderSub.TLabel").pack(anchor="w")

        conn = ttk.Frame(header, style="Header.TFrame")
        conn.pack(side="right")
        self.status_pill = tk.Label(conn, text="●  Disconnected", bg=C["pill_off_bg"], fg=C["pill_off_fg"],
                                    font=(FONT, 9, "bold"), padx=10, pady=4)
        self.status_pill.pack(side="right", padx=(12, 0))
        self.conn_btn = ttk.Button(conn, text="Connect", style="Accent.TButton", command=self.toggle_connect,
                                   width=12)
        self.conn_btn.pack(side="right", padx=(10, 0))
        self.baud_var = tk.StringVar(value=str(self.prefs.get("baud", DEFAULT_BAUD)))
        self.baud_cb = ttk.Combobox(conn, textvariable=self.baud_var, width=8, style="Header.TCombobox",
                                    values=["115200", "230400", "460800", "921600"], state="readonly")
        self.baud_cb.pack(side="right")
        ttk.Label(conn, text="Baud", style="Header.TLabel").pack(side="right", padx=(12, 6))
        self.refresh_btn = ttk.Button(conn, text="↻", width=3, style="Header.TButton", command=self.refresh_ports)
        self.refresh_btn.pack(side="right", padx=(6, 0))
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(conn, textvariable=self.port_var, width=30, state="readonly",
                                    style="Header.TCombobox")
        self.port_cb.pack(side="right")
        ttk.Label(conn, text="Port", style="Header.TLabel").pack(side="right", padx=(0, 6))
        self.bind("<Return>", lambda e: None if self.link.is_open else self.toggle_connect())

        # ---- hint banner (visible while disconnected)
        self.hint = tk.Frame(self, bg=C["accent_soft"])
        tk.Label(self.hint, text="①", bg=C["accent_soft"], fg=C["accent_text"], font=(FONT, 14, "bold")).pack(
            side="left", padx=(18, 8), pady=6)
        self.hint_lbl = tk.Label(self.hint, bg=C["accent_soft"], fg=C["text"], font=(FONT, 10),
                                 text="Plug in the programmer, choose its COM port (e.g. USB-SERIAL CH340) "
                                      "and press Connect.")
        self.hint_lbl.pack(side="left")
        self.hint.pack(fill="x")

        # ---- body
        body = ttk.Frame(self, padding=(16, 14, 16, 14))
        body.pack(fill="both", expand=True)
        body.columnconfigure(0, weight=5, uniform="col")
        body.columnconfigure(1, weight=6, uniform="col")
        body.rowconfigure(1, weight=1, minsize=190)
        left = ttk.Frame(body)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        right = ttk.Frame(body)
        right.grid(row=0, column=1, sticky="nsew", padx=(8, 0))

        self._build_device_card(left)
        self._build_file_card(left)
        self._build_settings_card(left)
        self._build_status_card(right)
        self._build_counter_card(right)
        self._build_tools_card(right)
        self._build_log(body)

    def _build_device_card(self, parent):
        card = self._card(parent, "On the programmer", "2", action=("Refresh", self.refresh_device))
        card.pack(fill="x")
        b = card.body
        self.dev_name = ttk.Label(b, text="—", style="FileName.TLabel")
        self.dev_name.pack(anchor="w")
        self.dev_detail = ttk.Label(b, text="Connect to see the stored firmware.", style="Muted.TLabel")
        self.dev_detail.pack(anchor="w", pady=(2, 6))
        self.match_lbl = tk.Label(b, text="", font=(FONT, 9, "bold"), bg=C["card"], padx=8, pady=2)
        self.match_lbl.pack(anchor="w")

    def _build_file_card(self, parent):
        card = self._card(parent, "Load new firmware", "3")
        card.pack(fill="x", pady=10)
        b = card.body
        row = ttk.Frame(b, style="Card.TFrame")
        row.pack(fill="x")
        self.path_var = tk.StringVar(value="No file selected")
        ttk.Label(row, textvariable=self.path_var, style="Card.TLabel", font=(FONT, 10, "bold")).pack(
            side="left", fill="x", expand=True)
        ttk.Button(row, text="Browse…", command=self.browse).pack(side="right")
        self.img_lbl = ttk.Label(b, text="Intel HEX (.hex) or S-record (.srec / .mot) from e2 studio",
                                 style="Muted.TLabel")
        self.img_lbl.pack(anchor="w", pady=(2, 8))
        row = ttk.Frame(b, style="Card.TFrame")
        row.pack(fill="x")
        self.upload_btn = self._track(ttk.Button(row, text="Upload to programmer", style="Accent.TButton",
                                                 command=self.upload))
        self.upload_btn.pack(side="right")
        self.up_prog = ttk.Progressbar(b, maximum=100)
        self.up_prog.pack(fill="x", pady=(10, 0))

    def _build_settings_card(self, parent):
        card = self._card(parent, "Programmer settings")
        card.pack(fill="both", expand=True)
        b = card.body
        b.columnconfigure(1, weight=1)

        def label(row, text):
            ttk.Label(b, text=text, style="Card.TLabel").grid(row=row, column=0, sticky="w", padx=(0, 18), pady=3)

        def cell(row):
            f = ttk.Frame(b, style="Card.TFrame")
            f.grid(row=row, column=1, sticky="w", pady=3)
            return f

        def seg(row, text, var, options):
            label(row, text)
            f = cell(row)
            for value, opt in options:
                self._track(ttk.Radiobutton(f, text=opt, value=value, variable=var, width=17,
                                            style="Seg.Toolbutton")).pack(side="left")

        self.mode_var = tk.StringVar(value="button")
        self.erase_var = tk.StringVar(value="chip")
        seg(0, "Start flashing by", self.mode_var, [("button", "Button press"), ("auto", "Auto-detect board")])
        seg(1, "Erase", self.erase_var, [("chip", "Full chip"), ("blocks", "Only image blocks")])

        label(2, "After programming")
        checks = cell(2)
        self.verify_var = tk.BooleanVar(value=True)
        self.run_var = tk.BooleanVar(value=True)
        self._track(ttk.Checkbutton(checks, text="Verify", variable=self.verify_var,
                                    style="Card.TCheckbutton")).pack(side="left", padx=(0, 16))
        self._track(ttk.Checkbutton(checks, text="Run firmware", variable=self.run_var,
                                    style="Card.TCheckbutton")).pack(side="left", padx=(0, 16))

        label(3, "Reset line (RES)")
        f = cell(3)
        self.reset_var = tk.BooleanVar(value=True)
        self._track(ttk.Checkbutton(f, text="Use RES (connect under reset)", variable=self.reset_var,
                                    style="Card.TCheckbutton")).pack(side="left")

        def number(row, text, var, lo, hi, step, hint):
            label(row, text)
            f = cell(row)
            self._track(ttk.Spinbox(f, from_=lo, to=hi, increment=step, width=7, textvariable=var)).pack(
                side="left")
            ttk.Label(f, text=hint, style="Muted.TLabel").pack(side="left", padx=(10, 0))

        self.swdelay_var = tk.StringVar(value="4")
        self.settle_var = tk.StringVar(value="500")
        number(4, "SWD clock delay", self.swdelay_var, 0, 250, 1, "0 = fastest; raise for long wires")
        number(5, "Auto-detect settle", self.settle_var, 0, 10000, 100, "ms to wait after a board is detected")

        tk.Frame(b, bg=C["border"], height=1).grid(row=6, column=0, columnspan=2, sticky="ew", pady=(8, 6))
        foot = ttk.Frame(b, style="Card.TFrame")
        foot.grid(row=7, column=0, columnspan=2, sticky="ew")
        self.saved_lbl = tk.Label(foot, text="", bg=C["card"], fg=C["muted"], font=(FONT, 9, "bold"))
        self.saved_lbl.pack(side="left")
        self.save_btn = ttk.Button(foot, text="Save to programmer", style="Accent.TButton", command=self.save_settings)
        self.save_btn.pack(side="right")
        self.revert_btn = ttk.Button(foot, text="Undo changes", command=self.revert_settings)
        self.revert_btn.pack(side="right", padx=(0, 8))

        self._saved_settings = None
        self.setting_vars = {"mode": self.mode_var, "erase": self.erase_var, "verify": self.verify_var,
                             "run": self.run_var, "reset": self.reset_var, "swdelay": self.swdelay_var,
                             "settle": self.settle_var}
        for var in self.setting_vars.values():
            var.trace_add("write", lambda *a: self._update_settings_state())

    def _build_status_card(self, parent):
        outer = tk.Frame(parent, bg=C["card"], highlightthickness=1, highlightbackground=C["border"])
        outer.pack(fill="x")
        self.banner = tk.Frame(outer, bg=C["idle_soft"])
        self.banner.pack(fill="x")
        self.state_lbl = tk.Label(self.banner, text="Not connected", bg=C["idle_soft"], fg=C["muted"],
                                  font=(FONT, 24, "bold"), anchor="w")
        self.state_lbl.pack(fill="x", padx=18, pady=(14, 0))
        self.result_lbl = tk.Label(self.banner, text="", bg=C["idle_soft"], fg=C["muted"], font=(FONT, 10),
                                   anchor="w", justify="left", wraplength=560)
        self.result_lbl.pack(fill="x", padx=18, pady=(2, 12))
        inner = ttk.Frame(outer, style="Card.TFrame", padding=(18, 12, 18, 14))
        inner.pack(fill="x")
        self.prog = ttk.Progressbar(inner, maximum=100)
        self.prog.pack(fill="x")
        self.flash_btn = self._track(ttk.Button(inner, text="⚡  FLASH TARGET", style="Flash.TButton",
                                                command=self.flash_now))
        self.flash_btn.pack(fill="x", pady=(12, 4))
        self.flash_hint = ttk.Label(inner, text="", style="Muted.TLabel")
        self.flash_hint.pack(anchor="w")

    def _build_counter_card(self, parent):
        card = self._card(parent, "Production counters", action=("Reset counters", self.reset_counters))
        card.pack(fill="x", pady=10)
        b = card.body
        for c in range(4):
            b.columnconfigure(c, weight=1, uniform="cnt")

        def tile(col, title, fg, bg):
            f = tk.Frame(b, bg=bg)
            f.grid(row=0, column=col, sticky="nsew", padx=4)
            tk.Label(f, text=title, bg=bg, fg=fg, font=(FONT, 9, "bold")).pack(pady=(8, 0))
            lbl = tk.Label(f, text="0", bg=bg, fg=fg, font=(FONT, 26, "bold"))
            lbl.pack(pady=(0, 6))
            return lbl

        self.pass_lbl = tile(0, "PASS", C["ok_text"], C["ok_soft"])
        self.fail_lbl = tile(1, "FAIL", C["err_text"], C["err_soft"])
        self.total_lbl = tile(2, "TOTAL", C["text"], C["idle_soft"])
        self.yield_lbl = tile(3, "YIELD", C["accent_text"], C["accent_soft"])

    def _build_tools_card(self, parent):
        card = self._card(parent, "Target tools")
        card.pack(fill="both", expand=True)
        b = card.body
        for c in range(3):
            b.columnconfigure(c, weight=1)
        items = [("Detect chip", self.detect), ("Reset & run target", self.reset_target),
                 ("Erase chip", self.erase_chip)]
        for i, (txt, fn) in enumerate(items):
            self._track(ttk.Button(b, text=txt, command=fn)).grid(row=0, column=i, sticky="ew", padx=3, pady=3)

    def _build_log(self, parent):
        card = self._card(parent, "Activity log")
        card.grid(row=1, column=0, columnspan=2, sticky="nsew", pady=(10, 0))
        b = card.body
        cmdrow = ttk.Frame(b, style="Card.TFrame")
        cmdrow.pack(side="bottom", fill="x", pady=(6, 0))
        ttk.Label(cmdrow, text="Command", style="Muted.TLabel").pack(side="left")
        self.cmd_var = tk.StringVar()
        cmd_entry = self._track(ttk.Entry(cmdrow, textvariable=self.cmd_var, font=(MONO, 9)))
        cmd_entry.pack(side="left", fill="x", expand=True, padx=6)
        cmd_entry.bind("<Return>", lambda e: self.send_raw())
        self._track(ttk.Button(cmdrow, text="Send", command=self.send_raw)).pack(side="left")
        self.log = tk.Text(b, height=7, font=(MONO, 9), state="disabled", wrap="none", relief="flat",
                           bg=C["log_bg"], fg=C["text"], padx=8, pady=6, highlightthickness=0,
                           insertbackground=C["text"], selectbackground=C["accent"])
        sb = ttk.Scrollbar(b, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        self.log.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.log.tag_configure("err", foreground=C["err_text"])
        self.log.tag_configure("ok", foreground=C["ok_text"])
        self.log.tag_configure("dim", foreground=C["muted"])
        self.log.tag_configure("time", foreground=C["log_time"])

    # ================================================================ helpers
    def write_log(self, text, tag=None):
        self.log.configure(state="normal")
        self.log.insert("end", f"{dt.datetime.now():%H:%M:%S}  ", "time")
        self.log.insert("end", f"{text}\n", tag)
        self.log.see("end")
        self.log.configure(state="disabled")

    def run_bg(self, title, fn, *args, quiet=False):
        """Run a blocking link operation off the UI thread."""
        if not self.link.is_open:
            if not quiet:
                messagebox.showwarning(APP_TITLE, "Connect to the programmer first.")
            return
        if self.busy:
            if not quiet:
                messagebox.showinfo(APP_TITLE, "Another operation is still running.")
            return
        self.busy = True

        def worker():
            try:
                fn(*args)
            except Exception as e:  # noqa: BLE001 - report everything to the user
                self.ui_q.put(("error", f"{title}: {e}"))
            finally:
                self.ui_q.put(("idle", None))

        threading.Thread(target=worker, daemon=True).start()

    def call(self, fn, *args, **kwargs):
        """Schedule fn on the UI thread from a worker."""
        self.ui_q.put(("call", (fn, args, kwargs)))

    def _set_banner(self, title, msg="", kind="idle"):
        bg, fg = {"idle": (C["idle_soft"], C["text"]), "busy": (C["accent_soft"], C["accent_text"]),
                  "ok": (C["ok_soft"], C["ok_text"]), "err": (C["err_soft"], C["err_text"]),
                  "off": (C["idle_soft"], C["muted"])}[kind]
        for w in (self.banner, self.state_lbl, self.result_lbl):
            w.configure(bg=bg)
        self.state_lbl.configure(text=title, fg=fg)
        self.result_lbl.configure(text=msg, fg=fg if kind in ("ok", "err") else C["muted"])
        self.prog.configure(style={"ok": "Ok.Horizontal.TProgressbar",
                                   "err": "Err.Horizontal.TProgressbar"}.get(kind, "Horizontal.TProgressbar"))

    def _update_flash_hint(self):
        if not self.link.is_open:
            self.flash_hint.configure(text="Connect the programmer first.")
        elif self.dev_info.get("img") != "1":
            self.flash_hint.configure(text="No firmware on the programmer yet — upload one first (step 3).")
        elif (self._saved_settings or {}).get("mode") == "auto":
            self.flash_hint.configure(text="Auto-detect is on: place a board on the fixture and it is flashed "
                                           "automatically.  F5 flashes now.")
        else:
            self.flash_hint.configure(text="Press FLASH (F5) or the button on the programmer.")

    def _update_enabled(self):
        on = self.link.is_open and not self.device_busy
        for w in self.conn_widgets:
            try:
                if isinstance(w, ttk.Combobox):
                    w.state(["!disabled", "readonly"] if on else ["disabled"])
                else:
                    w.state(["!disabled"] if on else ["disabled"])
            except tk.TclError:
                pass
        self._update_upload_button()
        self._update_settings_state()

    # ================================================================ connection
    def refresh_ports(self):
        ports = sorted((p for p in serial.tools.list_ports.comports() if not is_bluetooth_port(p)),
                       key=lambda p: p.device)
        values = [f"{p.device} - {p.description}" for p in ports]
        self.port_cb["values"] = values
        want = self.prefs.get("port")
        current = self.port_var.get()
        if current not in values:
            match = next((v for v in values if want and v.startswith(want + " ")), None)
            self.port_var.set(match or (values[0] if values else ""))
        if not values:
            self.port_var.set("")
            self.hint_lbl.configure(text="No serial port found. Plug in the programmer's USB cable, then press ⟳.")

    def toggle_connect(self):
        if self.link.is_open:
            self.link.close()
            self._set_connected(False)
            return
        port = self.port_var.get().split(" - ")[0]
        if not port:
            messagebox.showwarning(APP_TITLE, "Select the programmer's COM port first.")
            return
        try:
            self.link.open(port, int(self.baud_var.get()))
        except Exception as e:
            messagebox.showerror(APP_TITLE, f"Cannot open {port}:\n{e}\n\nIs another program (serial monitor, "
                                            f"PlatformIO upload) using the port?")
            return
        self.prefs.update(port=port, baud=int(self.baud_var.get()))
        save_prefs(self.prefs)
        self.conn_btn.configure(text="Connecting…", style="Accent.TButton")
        self.conn_btn.state(["disabled"])
        self.run_bg("Connect", self._handshake)

    def _handshake(self):
        time.sleep(0.3)
        ver = None
        for _ in range(3):
            try:
                ver = self.link.request("PING", timeout=1.5)
                break
            except LinkError:
                pass
        if ver is None:
            self.call(self._connect_failed)
            raise LinkError("the programmer did not answer. Check the COM port and baud rate "
                            "(the firmware uses 460800).")
        if not ver.startswith("RAFLASHER "):
            self.call(self._connect_failed)
            raise LinkError(f"this is not the RA programmer firmware ({ver}). Upload the flasher_renesas "
                            f"firmware with PlatformIO first.")
        self.ui_q.put(("log", (f"Connected to {ver}", "ok")))
        self.call(self._set_connected, True)
        self._read_device()

    def _read_device(self):
        """(worker) Read INFO and show it."""
        info = parse_kv(self.link.request("INFO"))
        self.call(self._apply_device_info, info)

    def _connect_failed(self):
        self.link.close()
        self._set_connected(False)

    def refresh_device(self):
        self.run_bg("Refresh", self._read_device, quiet=True)

    def _set_connected(self, on):
        self.conn_btn.state(["!disabled"])
        self.port_cb.state(["disabled"] if on else ["!disabled", "readonly"])
        self.baud_cb.state(["disabled"] if on else ["!disabled", "readonly"])
        self.refresh_btn.state(["disabled"] if on else ["!disabled"])
        if on:
            self.conn_btn.configure(text="Disconnect", style="TButton")
            self.status_pill.configure(text="●  Connected", bg=C["pill_on_bg"], fg=C["pill_on_fg"])
            self.hint.pack_forget()
            self._set_banner("Ready", "Programmer connected.", "idle")
        else:
            self.device_busy = False
            self.conn_btn.configure(text="Connect", style="Accent.TButton")
            self.status_pill.configure(text="●  Disconnected", bg=C["pill_off_bg"], fg=C["pill_off_fg"])
            if not self.hint.winfo_ismapped():
                self.hint.pack(fill="x", after=self.winfo_children()[0])
            self._set_banner("Not connected", "Connect the programmer to begin.", "off")
            self.dev_info = {}
            self._saved_settings = None
            self.dev_name.configure(text="—")
            self.dev_detail.configure(text="Connect to see the stored firmware.")
            self.match_lbl.configure(text="", bg=C["card"])
        self._update_enabled()

    def _pulse(self):
        """Gently pulse the Connect button while disconnected so it is the obvious first step."""
        if not self.link.is_open and "disabled" not in self.conn_btn.state():
            self.pulse_on = not self.pulse_on
            self.conn_btn.configure(style="Pulse.TButton" if self.pulse_on else "Accent.TButton")
        self.after(700, self._pulse)

    def _apply_device_info(self, info):
        self.dev_info = info
        if info.get("img") == "1":
            name = info.get("name", "-")
            cf, df = int(info.get("cf", 0)), int(info.get("df", 0))
            what = f"Code flash {cf:,} B" + (f" + data flash {df:,} B" if df else "")
            self.dev_name.configure(text=name if name != "-" else "(unnamed image)")
            self.dev_detail.configure(text=f"{what}   ·   CRC {info.get('crc')}   ·   fw {info.get('fw')}")
            self.write_log(f"Programmer holds {name} ({what}, CRC {info.get('crc')})")
        else:
            self.dev_name.configure(text="No firmware stored")
            self.dev_detail.configure(text="Load a .hex / .srec file below and upload it to the programmer.")

        if "mode" in info:
            self._saved_settings = {k: info.get(k, SETTING_DEFAULTS[k]) for k in SETTING_KEYS}
            self._show_settings(self._saved_settings)
        self._set_counts(int(info.get("pass", 0)), int(info.get("fail", 0)))
        self._update_match()
        self._update_enabled()

    def _set_counts(self, p, f):
        self.pass_lbl.configure(text=str(p))
        self.fail_lbl.configure(text=str(f))
        self.total_lbl.configure(text=str(p + f))
        self.yield_lbl.configure(text=f"{100.0 * p / (p + f):.0f}%" if p + f else "–")

    def _stored_crc(self):
        try:
            return int(self.dev_info.get("crc", "0"), 16) if self.dev_info.get("img") == "1" else None
        except ValueError:
            return None

    def _same_data(self):
        return bool(self.image) and self._stored_crc() == self.image[2]

    def _update_match(self):
        if not self.link.is_open or not self.image or self._stored_crc() is None:
            self.match_lbl.configure(text="", bg=C["card"])
        elif self._same_data():
            self.match_lbl.configure(text="✓  Same as the file selected below", bg=C["ok_soft"], fg=C["ok_text"])
        else:
            self.match_lbl.configure(text="⚠  Different from the file selected below — upload to replace it",
                                     bg=C["warn_soft"], fg=C["warn"])
        self._update_upload_button()

    def _update_upload_button(self):
        if self._same_data():
            self.upload_btn.configure(text="Upload again", style="TButton")
        else:
            self.upload_btn.configure(text="Upload to programmer", style="Accent.TButton")
        if not self.image:
            self.upload_btn.state(["disabled"])
        elif self.link.is_open and not self.device_busy:
            self.upload_btn.state(["!disabled"])

    # ================================================================ image
    def browse(self):
        start = os.path.dirname(self.prefs.get("last_file", "")) or None
        path = filedialog.askopenfilename(
            title="Select firmware", initialdir=start,
            filetypes=[("Firmware", "*.hex *.srec *.mot *.s19 *.s28 *.s37 *.bin"), ("All files", "*.*")])
        if path:
            self._load_file(path)

    def _load_file(self, path, quiet=False):
        try:
            im = load_image(path)
        except Exception as e:
            if not quiet:
                messagebox.showerror(APP_TITLE, f"Cannot load {os.path.basename(path)}:\n{e}")
            return
        crc = zlib.crc32(im["data"])
        self.image = (path, im, crc)
        self.path_var.set(os.path.basename(path))
        self.img_lbl.configure(text=f"{describe_image(im)}   ·   CRC 0x{crc:08X}")
        self.prefs["last_file"] = path
        save_prefs(self.prefs)
        self.write_log(f"Selected {os.path.basename(path)}: {describe_image(im)}")
        self._update_match()
        self._update_enabled()

    def upload(self):
        if not self.image:
            messagebox.showwarning(APP_TITLE, "Select a firmware file first.")
            return
        path, im, crc = self.image
        try:  # the file may have been rebuilt since it was selected
            fresh = load_image(path)
            if zlib.crc32(fresh["data"]) != crc:
                self._load_file(path)
                path, im, crc = self.image
                self.write_log("The file changed on disk — re-loaded it before uploading", "dim")
        except Exception:
            pass
        self.run_bg("Upload", self._upload_worker, path, im)

    def _upload_worker(self, path, im):
        name = os.path.basename(path).replace(" ", "_")[:40]
        data = im["data"]
        crc = zlib.crc32(data)
        self.link.request(f"BEGIN {len(data)} 0x{crc:08X} {len(im['cf'])} {len(im['df'])} {name}")
        t0 = time.time()
        try:
            for off in range(0, len(data), CHUNK):
                self.link.request(f"DATA {off} {data[off:off + CHUNK].hex()}", timeout=3)
                self.ui_q.put(("upload", min(100, (off + CHUNK) * 100 // len(data))))
            info = self.link.request("END", timeout=5)
        except Exception:
            try:
                self.link.request("ABORT")
            except LinkError:
                pass
            raise
        self.ui_q.put(("log", (f"Uploaded {name}: {len(data):,} bytes in {time.time() - t0:.1f}s, "
                               f"CRC 0x{crc:08X} verified", "ok")))
        self.call(self._apply_device_info, parse_kv(info))

    # ================================================================ settings
    def _current_settings(self):
        out = {}
        for key in SETTING_KEYS:
            var = self.setting_vars[key]
            if isinstance(var, tk.BooleanVar):
                out[key] = str(int(var.get()))
            elif key in ("mode", "erase"):
                out[key] = var.get()
            else:
                try:
                    out[key] = str(int(var.get()))
                except ValueError:
                    out[key] = SETTING_DEFAULTS[key]
        return out

    def _show_settings(self, st):
        self.suppress_set = True
        try:
            for key, value in st.items():
                var = self.setting_vars[key]
                var.set(value == "1" if isinstance(var, tk.BooleanVar) else value)
        finally:
            self.suppress_set = False
        self._update_settings_state()

    def _changed_settings(self):
        if not self._saved_settings:
            return {}
        cur = self._current_settings()
        return {k: v for k, v in cur.items() if self._saved_settings.get(k) != v}

    def _update_settings_state(self):
        if self.suppress_set:
            return
        self._update_flash_hint()
        changed = self._changed_settings()
        ready = self.link.is_open and not self.device_busy and self._saved_settings is not None
        if not ready:
            self.saved_lbl.configure(text="Connect to view and change the programmer settings.", fg=C["muted"])
        elif changed:
            n = len(changed)
            self.saved_lbl.configure(text=f"●  {n} unsaved change{'s' if n > 1 else ''}", fg=C["warn"])
        else:
            self.saved_lbl.configure(text="✓  Saved on the programmer", fg=C["ok_text"])
        for btn in (self.save_btn, self.revert_btn):
            btn.state(["!disabled"] if ready and changed else ["disabled"])

    def save_settings(self):
        changed = self._changed_settings()
        if not changed:
            return

        def work():
            for key, value in changed.items():
                self.link.request(f"SET {key} {value}")
            info = parse_kv(self.link.request("INFO"))
            self.ui_q.put(("log", ("Settings saved: " + ", ".join(f"{k}={v}" for k, v in changed.items()), "ok")))
            self.call(self._apply_device_info, info)

        self.run_bg("Save settings", work)

    def revert_settings(self):
        if self._saved_settings:
            self._show_settings(self._saved_settings)

    # ================================================================ tools
    def flash_now(self):
        if self.device_busy:
            return
        if self.link.is_open and self.dev_info.get("img") != "1":
            messagebox.showinfo(APP_TITLE, "The programmer has no firmware yet. Upload one first.")
            return
        self.run_bg("Flash", lambda: self.link.request("FLASH"))

    def detect(self):
        def work():
            info = parse_kv(self.link.request("DETECT", timeout=10))
            known = info.get("part") != "UNKNOWN"
            msg = (f"{info.get('part')}  ·  {info.get('flash')} KB code flash  ·  UID {info.get('uid')}  ·  "
                   f"SWD delay {info.get('swdelay')}" + ("  ·  via RES" if info.get("reset") == "1" else ""))
            self.ui_q.put(("log", (f"Detected {msg}  ·  DPIDR {info.get('dpidr')}  ·  access window "
                                   f"{info.get('aws')}/{info.get('awe')}", "ok")))
            self.call(self._set_banner, "Chip detected" if known else "Chip answers, part number unknown", msg,
                      "ok" if known else "err")

        self.run_bg("Detect", work)

    def reset_target(self):
        def work():
            self.link.request("RESET", timeout=10)
            self.ui_q.put(("log", ("Target reset and released — the firmware runs", "ok")))
        self.run_bg("Reset", work)

    def erase_chip(self):
        if messagebox.askyesno(APP_TITLE, "Erase the whole code flash and data flash of the connected target?"):
            def work():
                self.link.request("ERASE", timeout=30)
                self.ui_q.put(("log", ("Target erased", "ok")))
            self.run_bg("Erase", work)

    def send_raw(self):
        """Send a raw protocol command (e.g. 'READ 0x00000000 16') and log the reply."""
        cmd = self.cmd_var.get().strip()
        if not cmd:
            return
        self.cmd_var.set("")
        self.write_log(f"> {cmd}", "dim")

        def work():
            reply = self.link.request(cmd, timeout=30)
            self.ui_q.put(("log", (reply or "OK", "ok")))
        self.run_bg(cmd.split()[0].upper(), work)

    def reset_counters(self):
        if messagebox.askyesno(APP_TITLE, "Reset PASS / FAIL counters on the programmer?"):
            self.run_bg("Reset counters", lambda: self.link.request("RESETCNT"))

    # ================================================================ events
    def _drain(self):
        try:
            while True:
                item = self.ui_q.get_nowait()
                if isinstance(item, str):
                    self._on_device_line(item)
                    continue
                kind, val = item
                if kind == "log":
                    self.write_log(*val)
                elif kind == "error":
                    self.write_log(val, "err")
                    messagebox.showerror(APP_TITLE, val)
                elif kind == "idle":
                    self.busy = False
                elif kind == "upload":
                    self.up_prog["value"] = val
                elif kind == "call":
                    fn, args, kwargs = val
                    fn(*args, **kwargs)
        except queue.Empty:
            pass
        self.after(50, self._drain)

    def _set_device_busy(self, busy):
        if busy != self.device_busy:
            self.device_busy = busy
            self._update_enabled()

    def _on_device_line(self, line):
        if not line.startswith("@EVT"):
            return  # boot noise from the ESP8266 ROM
        parts = line.split(" ", 3)
        ev = parts[1] if len(parts) > 1 else ""
        if ev == "COUNT":
            self._set_counts(int(parts[2]), int(parts[3]))
        elif ev == "STATE":
            st = parts[2]
            if st == "idle":
                self._set_device_busy(False)
                return
            self._set_device_busy(True)
            if st == "connect":
                self.prog["value"] = 0
            self._set_banner(self.STATE_TEXT.get(st, st) + "…", "", "busy")
        elif ev == "PROGRESS":
            self.prog["value"] = int(parts[3])
            self.result_lbl.configure(text=f"{parts[2].capitalize()}  {parts[3]}%")
        elif ev == "CHIP":
            self.write_log(f"Target: {parts[2]}  UID {parts[3]}")
        elif ev == "TARGET":
            if parts[2] == "present":
                self.write_log("Board detected (auto mode)", "dim")
                settle = (self._saved_settings or {}).get("settle", "500")
                self._set_banner("Board detected", f"Flashing starts in {settle} ms…", "busy")
            else:
                self.write_log("Board removed — waiting for the next one", "dim")
                self._set_banner("Waiting for board", "Place the next board on the fixture.", "idle")
        elif ev == "RESULT":
            self._on_result(line)
        elif ev == "BOOT":
            self.write_log("Programmer restarted", "dim")
            if self.link.is_open:
                self.refresh_device()
        elif ev == "LINKLOST":
            self.link.close()
            self._set_connected(False)
            self.write_log("Serial link lost — USB cable unplugged?", "err")
        else:
            self.write_log(line, "dim")

    def _on_result(self, line):
        parts = line.split(" ")
        img = self.dev_info.get("name", "")
        if parts[2] == "PASS":
            ms, chip, uid = parts[3], parts[4], parts[5]
            self.prog["value"] = 100
            verified = "programmed and verified" if (self._saved_settings or {}).get("verify") != "0" else \
                "programmed"
            msg = f"{chip} {verified} in {int(ms) / 1000:.2f} s"
            self._set_banner("✓  PASS", msg, "ok")
            self.write_log(f"PASS  {msg}", "ok")
            self._csv(["PASS", chip, uid, ms, "", img])
        else:
            counted = parts[3] == "1"
            reason = " ".join(parts[4:])
            self._set_banner("✕  FAIL", reason + ("" if counted else "   (not counted)"), "err")
            self.write_log(f"FAIL  {reason}" + ("" if counted else "  (not counted)"), "err")
            self._csv(["FAIL" if counted else "NO-TARGET", "", "", "", reason, img])
        self.bell()

    def _csv(self, row):
        try:
            new = not os.path.exists(LOG_CSV)
            with open(LOG_CSV, "a", newline="") as f:
                w = csv.writer(f)
                if new:
                    w.writerow(["timestamp", "result", "chip", "uid", "ms", "message", "image"])
                w.writerow([dt.datetime.now().isoformat(timespec="seconds")] + row)
        except OSError as e:
            self.write_log(f"Cannot write {LOG_CSV}: {e}", "err")

    def _on_close(self):
        self.link.close()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
