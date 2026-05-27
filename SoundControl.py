"""
Sound Control - Desktop companion for FabGL Sound Center
Converts audio files and streams to ESP32 over WiFi (16-bit PCM).
Made by @UfkuAcik
"""
import socket
import struct
import threading
import wave
import os
import glob
import random
import time
import subprocess

import customtkinter as ctk
import tkinter as tk
from tkinter import filedialog, messagebox

import shutil
if not shutil.which("ffmpeg"):
    local_appdata = os.environ.get('LOCALAPPDATA', '')
    if local_appdata:
        ffmpeg_dirs = glob.glob(os.path.join(local_appdata, r"Microsoft\WinGet\Packages\Gyan.FFmpeg*\*\bin"))
        if ffmpeg_dirs:
            os.environ["PATH"] += os.pathsep + ffmpeg_dirs[0]


STREAM_PORT = 8266

THEMES = {
    "Matrix": {"bg": "#000000", "fg": "#001a00", "accent": "#00cc00", "hover": "#009900"},
    "Classic Blue": {"bg": "#2b2b2b", "fg": "#ffffff", "accent": "#1f538d", "hover": "#14375e"},
    "Amber Retro": {"bg": "#1a0f00", "fg": "#2b1a00", "accent": "#ffb000", "hover": "#cc8c00"},
    "Dark": {"bg": "#121212", "fg": "#1e1e1e", "accent": "#888888", "hover": "#aaaaaa"},
    "Cyberpunk": {"bg": "#0e0e1a", "fg": "#1a1a2e", "accent": "#ff00ff", "hover": "#cc00cc"}
}

class CyberpunkCanvas(tk.Canvas):
    def __init__(self, master, **kwargs):
        kwargs['bg'] = '#0D0221'
        super().__init__(master, **kwargs)
        self.bind("<Configure>", self._on_resize)
        self.width = 800
        self.height = 600
        self.offset = 0.0
        self._animate()

    def _on_resize(self, event):
        self.width = event.width
        self.height = event.height

    def _animate(self):
        self.delete("all")
        horizon = self.height * 0.45
        cx = self.width / 2
        
        # Sun
        sun_r = 100
        self.create_oval(cx - sun_r, horizon - sun_r*1.1, cx + sun_r, horizon + sun_r*0.1, fill="#FF007F", outline="", tags="sun")
        
        # Grid lines (perspective)
        num_v_lines = 30
        spacing = self.width / 15
        for i in range(-num_v_lines, num_v_lines):
            x_bottom = cx + i * spacing * 4
            self.create_line(cx, horizon, x_bottom, self.height, fill="#00FFCC", width=1)
            
        # Grid lines (horizontal moving)
        self.offset += 0.04
        if self.offset > 1.0:
            self.offset -= 1.0
            
        num_h_lines = 25
        for i in range(num_h_lines):
            t = (i + self.offset) / num_h_lines
            y = horizon + (self.height - horizon) * (t ** 2.5)
            self.create_line(0, y, self.width, y, fill="#00FFCC", width=1)
            
        self.after(50, self._animate)

class MatrixRainCanvas(tk.Canvas):
    def __init__(self, master, **kwargs):
        kwargs['bg'] = 'black'
        super().__init__(master, **kwargs)
        self.bind("<Configure>", self._on_resize)
        self.width = 800
        self.height = 600
        self.font_size = 14
        self.drops = []
        self.chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$+-*/=%\"'#&_(),.;:?!\\|{}<>[]^~"
        self._animate()

    def _on_resize(self, event):
        self.width = event.width
        self.height = event.height
        cols = self.width // self.font_size
        if len(self.drops) < cols:
            self.drops.extend([random.randint(-20, 0) for _ in range(cols - len(self.drops))])

    def _animate(self):
        self.delete("all")
        for i in range(len(self.drops)):
            char = random.choice(self.chars)
            x = i * self.font_size
            y = self.drops[i] * self.font_size
            
            self.create_text(x, y, text=char, fill="#00FF41", font=("Consolas", self.font_size))
            
            if y > self.height and random.random() > 0.975:
                self.drops[i] = 0
            else:
                self.drops[i] += 1
                
        self.after(50, self._animate)


class SoundControlApp(ctk.CTk):
    def __init__(self):
        super().__init__()
        
        self.title("Sound Control")
        self.geometry("750x730")
        self.resizable(False, False)
        
        ctk.set_appearance_mode("dark")
        
        self.theme_name = ctk.StringVar(value="Matrix")
        self.esp_theme_name = ctk.StringVar(value="Matrix")
        self.esp_ip = ctk.StringVar(value="192.168.1.100")
        self.display_ip = ctk.StringVar(value="x")
        self.src_file = ctk.StringVar(value="")
        self.stream_file = ctk.StringVar(value="")
        self.status_text = ctk.StringVar(value="Ready")
        
        self.streaming = False
        self.stream_thread = None
        self.sock = None
        
        self.ctrl_sock = None
        self.ctrl_connected = False
        self.vu_level = ctk.DoubleVar(value=0)
        self.prog_level = ctk.DoubleVar(value=0)

        self.media_state = 0
        self.seek_req = -1


        self._build_ui()
        self._apply_theme("Matrix")

    def _build_ui(self):
        self.matrix_canvas = MatrixRainCanvas(self, highlightthickness=0)
        self.cyberpunk_canvas = CyberpunkCanvas(self, highlightthickness=0)
        
        header = ctk.CTkFrame(self, fg_color="transparent")
        header.pack(fill="x", padx=30, pady=(10, 0))
        ctk.CTkLabel(header, text="Sound Control", font=ctk.CTkFont(size=26, weight="bold")).pack(side="left")
        
        ctk.CTkLabel(header, text="ESP32 IP:").pack(side="left", padx=(30, 5))
        self.ip_entry = ctk.CTkEntry(header, textvariable=self.esp_ip, width=120, font=("Consolas", 12))
        self.ip_entry.pack(side="left")
        self.ctrl_btn = ctk.CTkButton(header, text="Connect Control", command=self._toggle_control, width=120)
        self.ctrl_btn.pack(side="left", padx=10)
        self.ctrl_status = ctk.CTkLabel(header, text="Disconnected", text_color="red")
        self.ctrl_status.pack(side="left", padx=(0, 20))

        self.theme_menu = ctk.CTkOptionMenu(header, variable=self.theme_name, values=list(THEMES.keys()), command=self._apply_theme, width=250)
        self.theme_menu.pack(side="right")
        ctk.CTkLabel(header, text="Theme:", font=ctk.CTkFont(size=14)).pack(side="right", padx=10)

        # Custom Tabs Container
        self.tab_buttons_frame = ctk.CTkFrame(self, fg_color="transparent")
        self.tab_buttons_frame.pack(pady=(15, 0))
        
        self.btn_tab_stream = ctk.CTkButton(self.tab_buttons_frame, text="Audio Conversion & Streaming", command=lambda: self._switch_tab("stream"))
        self.btn_tab_stream.pack(side="left", padx=5)
        
        self.btn_tab_remote = ctk.CTkButton(self.tab_buttons_frame, text="Remote Control", command=lambda: self._switch_tab("remote"))
        self.btn_tab_remote.pack(side="left", padx=5)

        self.stream_widgets = []
        self.remote_widgets = []

        self._build_ui_stream()
        self._build_ui_remote()
        
        self._switch_tab("stream")

        footer = ctk.CTkFrame(self, fg_color="transparent")
        footer.pack(fill="x", side="bottom", pady=5)
        ctk.CTkLabel(footer, text="made with ❤ by @UfkuAcik", font=ctk.CTkFont(size=12, slant="italic"), text_color="gray50").pack(anchor="center")

    def _toggle_en(self, idx):
        var = self.ch_vars[idx]['en']
        btn = self.ch_vars[idx]['en_btn']
        nv = 0 if var.get() else 1
        var.set(nv)
        btn.configure(text="Disable" if nv else "Enable", fg_color="#B22222" if nv else "#2B7A0B", hover_color="#8B0000" if nv else "#1E5C06")
        self._send_cmd(f"SET_CH_EN:{idx}:{nv}")

    def _cycle_tempo(self, idx):
        var = self.ch_vars[idx]['tempo_var']
        btn = self.ch_vars[idx]['tempo_btn']
        nv = (var.get() + 1) % 5
        var.set(nv)
        t_names = ["T: OFF", "T: 0.5s", "T: 1s", "T: 2s", "T: 4s"]
        btn.configure(text=t_names[nv])
        self._send_cmd(f"SET_CH_TEMPO:{idx}:{nv}")


    def _switch_tab(self, tab_name):
        self.current_tab = tab_name
        t = THEMES[self.theme_name.get()]
        
        for w, kwargs in self.stream_widgets:
            if "relx" in kwargs: w.place_forget()
            else: w.pack_forget()
        for w, kwargs in self.remote_widgets:
            if "relx" in kwargs: w.place_forget()
            else: w.pack_forget()
        
        if tab_name == "stream":
            for w, kwargs in self.stream_widgets:
                if "relx" in kwargs: w.place(**kwargs)
                else: w.pack(**kwargs)
            self.btn_tab_stream.configure(fg_color=t["accent"], hover_color=t["hover"])
            self.btn_tab_remote.configure(fg_color="gray30", hover_color="gray40")
        else:
            for w, kwargs in self.remote_widgets:
                if "relx" in kwargs: w.place(**kwargs)
                else: w.pack(**kwargs)
            self.btn_tab_remote.configure(fg_color=t["accent"], hover_color=t["hover"])
            self.btn_tab_stream.configure(fg_color="gray30", hover_color="gray40")

    def _build_ui_stream(self):
        w1 = ctk.CTkLabel(self, text="  1. Audio Conversion  ", font=ctk.CTkFont(size=16, weight="bold"))
        self.stream_widgets.append((w1, {"anchor": "w", "padx": 20, "pady": (5, 5)}))
        
        fr1 = ctk.CTkFrame(self, fg_color="transparent")
        self.stream_widgets.append((fr1, {"fill": "x", "padx": 30, "pady": 5}))
        self.file_entry = ctk.CTkEntry(fr1, textvariable=self.src_file, placeholder_text="Select an audio file...", font=("Consolas", 12))
        self.file_entry.pack(side="left", fill="x", expand=True)
        self.browse_btn = ctk.CTkButton(fr1, text="Browse", width=80, command=self._browse)
        self.browse_btn.pack(side="left", padx=(10, 0))
        
        fr2 = ctk.CTkFrame(self, fg_color="transparent")
        self.stream_widgets.append((fr2, {"fill": "x", "padx": 30, "pady": 5}))
        self.convert_btn = ctk.CTkButton(fr2, text="Convert to SD WAV (8-bit Mono)", command=self._convert)
        self.convert_btn.pack(side="left")
        self.info_label = ctk.CTkLabel(fr2, text="Supported: WAV, MP3, FLAC, OGG, M4A, AAC, WMA, AIFF, ALAC", text_color="gray60")
        self.info_label.pack(side="left", padx=15)

        w2 = ctk.CTkLabel(self, text="  2. Live WiFi Streaming  ", font=ctk.CTkFont(size=16, weight="bold"))
        self.stream_widgets.append((w2, {"anchor": "w", "padx": 20, "pady": (15, 5)}))
        
        wr_file = ctk.CTkFrame(self, fg_color="transparent")
        self.stream_widgets.append((wr_file, {"fill": "x", "padx": 30, "pady": 5}))
        self.stream_entry = ctk.CTkEntry(wr_file, textvariable=self.stream_file, placeholder_text="Select a converted _8bit.wav to stream...", font=("Consolas", 12))
        self.stream_entry.pack(side="left", fill="x", expand=True)
        self.stream_browse_btn = ctk.CTkButton(wr_file, text="Browse", width=80, command=self._stream_browse)
        self.stream_browse_btn.pack(side="left", padx=(10, 0))

        w3 = ctk.CTkLabel(self, text="  3. System Logs  ", font=ctk.CTkFont(size=16, weight="bold"))
        self.stream_widgets.append((w3, {"anchor": "w", "padx": 20, "pady": (15, 5)}))
        
        fr_logs = ctk.CTkFrame(self, fg_color="transparent")
        self.stream_widgets.append((fr_logs, {"fill": "x", "padx": 30, "pady": (0, 10)}))
        self.logs_box = ctk.CTkTextbox(fr_logs, font=("Consolas", 12), height=80)
        self.logs_box.pack(fill="x")
        self.logs_box.configure(state="disabled")

    def _log(self, msg):
        self.logs_box.configure(state="normal")
        self.logs_box.insert("end", "- " + msg + "\n")
        self.logs_box.see("end")
        self.logs_box.configure(state="disabled")

    def _build_ui_remote(self):
        # LEFT COLUMN: Channels 1-4
        self.ch_vars = []
        for i in range(4):
            base_y = 0.16 + i*0.13
            # Channel Box
            lbl = ctk.CTkLabel(self, text=f" Signal Generation Channel {i+1} ", font=("Consolas", 12, "bold"))
            self.remote_widgets.append((lbl, {"relx": 0.02, "rely": base_y}))
            
            en_var = ctk.IntVar(value=0)
            
            # Row 1: Vol Slider, Type ComboBox
            vol_lbl = ctk.CTkLabel(self, text="Vol:", width=30)
            self.remote_widgets.append((vol_lbl, {"relx": 0.02, "rely": base_y + 0.04}))
            vol_var = ctk.DoubleVar(value=100)
            vol_sl = ctk.CTkSlider(self, variable=vol_var, from_=0, to=127, command=lambda val, idx=i: self._send_cmd(f"SET_CH_VOL:{idx}:{int(val)}"))
            self.remote_widgets.append((vol_sl, {"relx": 0.06, "rely": base_y + 0.04, "relwidth": 0.25}))
            
            type_lbl = ctk.CTkLabel(self, text="Type:", width=35)
            self.remote_widgets.append((type_lbl, {"relx": 0.33, "rely": base_y + 0.04}))
            type_var = ctk.StringVar(value="Sine")
            types = ["Sine", "Square", "Triangle", "Sawtooth", "Noise", "Kick", "Snare", "HiHat", "Crash", "Tom", "Clap", "Cowbell", "Ride", "Woodblk", "Bongo", "Conga", "Tambor", "Shaker", "Laser", "Bell", "RimSh", "FlrTom", "Guiro", "Maracs", "808K", "808Cl", "Timbal", "Agogo", "TriHit", "FMBel", "Siren", "ZapDn", "Metal", "PwrK", "Buzz"]
            type_cb = ctk.CTkOptionMenu(self, variable=type_var, values=types, command=lambda val, idx=i: self._send_cmd(f"SET_CH_TYPE:{idx}:{types.index(val)}"))
            self.remote_widgets.append((type_cb, {"relx": 0.38, "rely": base_y + 0.04, "relwidth": 0.15}))
            
            # Row 2: Frq Slider, Enable Button, Tempo Button
            frq_lbl = ctk.CTkLabel(self, text="Frq:", width=30)
            self.remote_widgets.append((frq_lbl, {"relx": 0.02, "rely": base_y + 0.09}))
            freq_var = ctk.DoubleVar(value=200)
            freq_sl = ctk.CTkSlider(self, variable=freq_var, from_=20, to=8000, command=lambda val, idx=i: self._send_cmd(f"SET_CH_FREQ:{idx}:{int(val)}"))
            self.remote_widgets.append((freq_sl, {"relx": 0.06, "rely": base_y + 0.09, "relwidth": 0.25}))
            
            en_btn = ctk.CTkButton(self, text="Enable", fg_color="#2B7A0B", hover_color="#1E5C06", command=lambda idx=i: self._toggle_en(idx))
            self.remote_widgets.append((en_btn, {"relx": 0.33, "rely": base_y + 0.09, "relwidth": 0.10}))
            
            tempo_var = ctk.IntVar(value=0)
            tempo_btn = ctk.CTkButton(self, text="Tmp: OFF", fg_color="#444444", hover_color="#666666", command=lambda idx=i: self._cycle_tempo(idx))
            self.remote_widgets.append((tempo_btn, {"relx": 0.44, "rely": base_y + 0.09, "relwidth": 0.09}))
            
            self.ch_vars.append({'en': en_var, 'type': type_var, 'vol': vol_var, 'freq': freq_var, 'en_btn': en_btn, 'tempo_var': tempo_var, 'tempo_btn': tempo_btn})

        # RIGHT COLUMN: System (Aligned with Channel 1, Y ~ 0.22)
        base_s = 0.16
        s_lbl = ctk.CTkLabel(self, text=" System ", font=("Consolas", 12, "bold"))
        self.remote_widgets.append((s_lbl, {"relx": 0.58, "rely": base_s}))
        
        s_theme_lbl = ctk.CTkLabel(self, text="Theme:", anchor="w")
        self.remote_widgets.append((s_theme_lbl, {"relx": 0.58, "rely": base_s + 0.05}))
        s_theme_cb = ctk.CTkOptionMenu(self, variable=self.esp_theme_name, values=list(THEMES.keys()), command=self._set_esp_theme)
        self.remote_widgets.append((s_theme_cb, {"relx": 0.65, "rely": base_s + 0.05, "relwidth": 0.30}))
        
        s_wifi_lbl = ctk.CTkLabel(self, text="WiFi:", anchor="w")
        self.remote_widgets.append((s_wifi_lbl, {"relx": 0.58, "rely": base_s + 0.10}))
        self.s_ip_lbl = ctk.CTkLabel(self, textvariable=self.display_ip, fg_color="#1f538d", corner_radius=4)
        self.remote_widgets.append((self.s_ip_lbl, {"relx": 0.65, "rely": base_s + 0.10, "relwidth": 0.30}))
        
        s_rst_btn = ctk.CTkButton(self, text="Reset", fg_color="#B22222", hover_color="#8B0000", command=self._confirm_reset)
        self.remote_widgets.append((s_rst_btn, {"relx": 0.65, "rely": base_s + 0.15, "relwidth": 0.30}))

        # RIGHT COLUMN: Master (Aligned with Channel 3, Y ~ 0.48)
        base_m = 0.42
        m_lbl = ctk.CTkLabel(self, text=" Master ", font=("Consolas", 12, "bold"))
        self.remote_widgets.append((m_lbl, {"relx": 0.58, "rely": base_m}))
        
        self.master_en = ctk.IntVar(value=1)
        m_cb = ctk.CTkButton(self, text="EN", width=40, fg_color="#2B7A0B", command=lambda: self._toggle_master_en())
        self.remote_widgets.append((m_cb, {"relx": 0.58, "rely": base_m + 0.04, "relwidth": 0.08}))
        self.master_en_btn = m_cb
        
        m_vol_lbl = ctk.CTkLabel(self, text="Volume:", anchor="w")
        self.remote_widgets.append((m_vol_lbl, {"relx": 0.58, "rely": base_m + 0.09}))
        self.master_vol = ctk.DoubleVar(value=127)
        m_vol_sl = ctk.CTkSlider(self, variable=self.master_vol, from_=0, to=127, command=lambda val: self._send_cmd(f"SET_MASTER_VOL:{int(val)}"))
        self.remote_widgets.append((m_vol_sl, {"relx": 0.65, "rely": base_m + 0.09, "relwidth": 0.30}))

        m_vu_lbl = ctk.CTkLabel(self, text="VU Meter:", anchor="w")
        self.remote_widgets.append((m_vu_lbl, {"relx": 0.58, "rely": base_m + 0.14}))
        self.remote_vu = ctk.CTkProgressBar(self, variable=self.vu_level, height=14, progress_color="#00FF00")
        self.remote_widgets.append((self.remote_vu, {"relx": 0.65, "rely": base_m + 0.14, "relwidth": 0.30}))

        # BOTTOM ROW: Media Player Spans Full Width
        base_mp = 0.71
        mp_lbl = ctk.CTkLabel(self, text=" Media Player ", font=("Consolas", 12, "bold"))
        self.remote_widgets.append((mp_lbl, {"relx": 0.02, "rely": base_mp}))
        
        # Listbox on Far Left
        self.media_listbox = tk.Listbox(self, bg="#2b2b2b", fg="white", selectbackground="#1f538d")
        self.remote_widgets.append((self.media_listbox, {"relx": 0.02, "rely": base_mp + 0.05, "relwidth": 0.18, "relheight": 0.17}))
        self.media_listbox.bind("<<ListboxSelect>>", self._on_media_select)
        
        # Player Buttons (Ply, Pau, Stp, SD)
        btn_play = ctk.CTkButton(self, text="Ply", command=self._remote_play)
        self.remote_widgets.append((btn_play, {"relx": 0.21, "rely": base_mp + 0.05, "relwidth": 0.05}))
        self.btn_pause = ctk.CTkButton(self, text="Pau", command=self._remote_pause)
        self.remote_widgets.append((self.btn_pause, {"relx": 0.27, "rely": base_mp + 0.05, "relwidth": 0.05}))
        btn_stop = ctk.CTkButton(self, text="Stp", command=self._remote_stop)
        self.remote_widgets.append((btn_stop, {"relx": 0.33, "rely": base_mp + 0.05, "relwidth": 0.05}))
        
        self.media_src_var = ctk.IntVar(value=0)
        src_lbl = ctk.CTkLabel(self, text="Src", font=("Consolas", 12, "bold"))
        self.remote_widgets.append((src_lbl, {"relx": 0.395, "rely": base_mp}))
        btn_src = ctk.CTkButton(self, text="SD", command=self._toggle_media_src)
        self.remote_widgets.append((btn_src, {"relx": 0.39, "rely": base_mp + 0.05, "relwidth": 0.045}))
        self.btn_src = btn_src
        
        agc_lbl = ctk.CTkLabel(self, text="AGC", font=("Consolas", 12, "bold"))
        self.remote_widgets.append((agc_lbl, {"relx": 0.445, "rely": base_mp}))
        self.agc_var = ctk.IntVar(value=0)
        self.agc_btn = ctk.CTkButton(self, text="Fix", fg_color="#444444", command=self._toggle_agc)
        self.remote_widgets.append((self.agc_btn, {"relx": 0.44, "rely": base_mp + 0.05, "relwidth": 0.045}))

        # Bst and Ready label
        bst_lbl = ctk.CTkLabel(self, text="Bst:", anchor="w")
        self.remote_widgets.append((bst_lbl, {"relx": 0.22, "rely": base_mp + 0.12}))
        self.booster_var = ctk.DoubleVar(value=1)
        bst_sl = ctk.CTkSlider(self, variable=self.booster_var, from_=1, to=10, command=lambda val: self._send_cmd(f"SET_BOOST:{int(val)}"))
        self.remote_widgets.append((bst_sl, {"relx": 0.26, "rely": base_mp + 0.12, "relwidth": 0.16}))
        
        self.media_state_lbl = ctk.CTkLabel(self, text="Ready", text_color="yellow")
        self.remote_widgets.append((self.media_state_lbl, {"relx": 0.43, "rely": base_mp + 0.12}))

        # Time Slider
        pb_lbl = ctk.CTkLabel(self, text="Time:", anchor="w")
        self.remote_widgets.append((pb_lbl, {"relx": 0.22, "rely": base_mp + 0.18}))
        self.remote_pb = ctk.CTkSlider(self, variable=self.prog_level, from_=0, to=1, height=14, command=self._remote_seek)
        self.remote_widgets.append((self.remote_pb, {"relx": 0.26, "rely": base_mp + 0.18, "relwidth": 0.23}))

        # FX and Speed Column
        sfx_lbl = ctk.CTkLabel(self, text=" SFX1 / SFX2 ", font=("Consolas", 12, "bold"))
        self.remote_widgets.append((sfx_lbl, {"relx": 0.49, "rely": base_mp}))
        
        # SFX1 Row
        self.fx_en_var = ctk.IntVar(value=0)
        fx_en = ctk.CTkCheckBox(self, text="ON", variable=self.fx_en_var, checkbox_width=20, checkbox_height=20, command=lambda: self._send_cmd(f"SET_FX_EN:{self.fx_en_var.get()}"))
        self.remote_widgets.append((fx_en, {"relx": 0.51, "rely": base_mp + 0.05, "relwidth": 0.07}))

        fx_names = ["None", "Echo", "Revrb", "Dist", "Fuzz", "Trem", "BitC", "Decim", "Chor", "Flang", "Wah", "Ring", "Sub", "OctF", "Vibr", "Tele", "Slap", "Gate", "Phas", "Robot", "PtSh", "LoFi", "Tape", "Stut", "RvEch", "S&H", "Comb", "Formt", "Shmmr", "Radio", "WahF"]
        self.fx_var = ctk.StringVar(value="None")
        fx_cb = ctk.CTkOptionMenu(self, variable=self.fx_var, values=fx_names, command=lambda val: self._send_cmd(f"SET_FX_TYPE:{fx_names.index(val)}"))
        self.remote_widgets.append((fx_cb, {"relx": 0.58, "rely": base_mp + 0.05, "relwidth": 0.12}))

        # SFX2 Row
        self.fx_en_var2 = ctk.IntVar(value=0)
        fx_en2 = ctk.CTkCheckBox(self, text="ON", variable=self.fx_en_var2, checkbox_width=20, checkbox_height=20, command=lambda: self._send_cmd(f"SET_FX_EN2:{self.fx_en_var2.get()}"))
        self.remote_widgets.append((fx_en2, {"relx": 0.51, "rely": base_mp + 0.10, "relwidth": 0.07}))

        self.fx_var2 = ctk.StringVar(value="None")
        fx_cb2 = ctk.CTkOptionMenu(self, variable=self.fx_var2, values=fx_names, command=lambda val: self._send_cmd(f"SET_FX_TYPE2:{fx_names.index(val)}"))
        self.remote_widgets.append((fx_cb2, {"relx": 0.58, "rely": base_mp + 0.10, "relwidth": 0.12}))

        # Overdrive & Speed buttons (side by side)
        self.od_en = 0
        self.sp_val = 1.0
        self.od_btn = ctk.CTkButton(self, text="OD: OFF", fg_color="#444444", command=self._toggle_overdrive)
        self.remote_widgets.append((self.od_btn, {"relx": 0.60, "rely": base_mp + 0.16, "relwidth": 0.10}))
        self.sp_btn = ctk.CTkButton(self, text="Spd: 1x", fg_color="#444444", command=self._toggle_speed)
        self.remote_widgets.append((self.sp_btn, {"relx": 0.51, "rely": base_mp + 0.16, "relwidth": 0.09}))

        # EQ Sliders (L, LM, M, HM, H)
        eq_main_lbl = ctk.CTkLabel(self, text=" Equalizer ", font=("Consolas", 12, "bold"))
        self.remote_widgets.append((eq_main_lbl, {"relx": 0.71, "rely": base_mp}))
        
        self.eq_l_var = ctk.DoubleVar(value=100)
        self.eq_lm_var = ctk.DoubleVar(value=100)
        self.eq_m_var = ctk.DoubleVar(value=100)
        self.eq_hm_var = ctk.DoubleVar(value=100)
        self.eq_h_var = ctk.DoubleVar(value=100)
        
        eq_vars = [
            (self.eq_l_var, "L", 0.73, lambda val: self._send_cmd(f"SET_EQ_LOW:{int(val)}")),
            (self.eq_lm_var, "LM", 0.78, lambda val: self._send_cmd(f"SET_EQ_LOWMID:{int(val)}")),
            (self.eq_m_var, "M", 0.83, lambda val: self._send_cmd(f"SET_EQ_MID:{int(val)}")),
            (self.eq_hm_var, "HM", 0.88, lambda val: self._send_cmd(f"SET_EQ_HIGHMID:{int(val)}")),
            (self.eq_h_var, "H", 0.93, lambda val: self._send_cmd(f"SET_EQ_HIGH:{int(val)}"))
        ]
        
        for var, label, x, cmd in eq_vars:
            sl = ctk.CTkSlider(self, from_=0, to=200, orientation="vertical", variable=var, command=cmd)
            self.remote_widgets.append((sl, {"relx": x, "rely": base_mp + 0.03, "relheight": 0.17, "relwidth": 0.02}))
            lbl = ctk.CTkLabel(self, text=label, font=("Consolas", 10))
            self.remote_widgets.append((lbl, {"relx": x, "rely": base_mp + 0.21}))


    def _toggle_master_en(self):
        nv = 0 if self.master_en.get() else 1
        self.master_en.set(nv)
        self.master_en_btn.configure(text="EN" if nv else "DIS", fg_color="#2B7A0B" if nv else "#B22222", hover_color="#1E5C06" if nv else "#8B0000")
        self._send_cmd(f"SET_MASTER_EN:{nv}")

    def _on_media_select(self, event):
        sel = self.media_listbox.curselection()
        if sel:
            self._send_cmd(f"MEDIA_SELECT:{sel[0]}")

    def _toggle_control(self):
        if self.ctrl_connected:
            self.ctrl_connected = False
            self.display_ip.set("x")
            if self.ctrl_sock:
                self.ctrl_sock.close()
                self.ctrl_sock = None
            self.ctrl_btn.configure(text="Connect Control")
            self.ctrl_status.configure(text="Disconnected", text_color="red")
        else:
            threading.Thread(target=self._control_worker, daemon=True).start()

    def _control_worker(self):
        ip = self.esp_ip.get()
        self.ctrl_status.configure(text="Connecting...", text_color="yellow")
        try:
            self.ctrl_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.ctrl_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.ctrl_sock.settimeout(5)
            self.ctrl_sock.connect((ip, 8267))
            self.ctrl_connected = True
            self.display_ip.set(ip)
            self.ctrl_btn.configure(text="Disconnect")
            self.ctrl_status.configure(text="Connected", text_color="green")
            self.ctrl_sock.settimeout(0.5)
            
            self._send_cmd("GET_DIR")
            time.sleep(0.1)
            self._send_cmd("GET_ALL_STATES")

            buffer = ""
            while self.ctrl_connected:
                try:
                    data = self.ctrl_sock.recv(4096).decode('utf-8', errors='ignore')
                    if not data: break
                    buffer += data
                    while '\n' in buffer:
                        line, buffer = buffer.split('\n', 1)
                        self.after(0, self._handle_telemetry, line.strip())
                except socket.timeout:
                    continue
                except Exception:
                    break
        except Exception as e:
            print("Control error:", e)
        finally:
            self.ctrl_connected = False
            self.display_ip.set("x")
            self.after(0, lambda: self.ctrl_status.configure(text="Disconnected", text_color="red"))
            self.after(0, lambda: self.ctrl_btn.configure(text="Connect Control"))
            if self.ctrl_sock:
                self.ctrl_sock.close()
                self.ctrl_sock = None

    def _handle_telemetry(self, line):
        if not line.startswith("TEL:"): return
        parts = line.split(":")
        if parts[1] == "VU":
            vu = int(parts[2])
            self.vu_level.set(vu / 100.0)
            if vu < 60: self.remote_vu.configure(progress_color="#00FF00")
            elif vu < 80: self.remote_vu.configure(progress_color="#FFFF00")
            else: self.remote_vu.configure(progress_color="#FF0000")
        elif parts[1] == "PROG":
            self.prog_level.set(int(parts[2]) / 100.0)
            self.seek_req = int(parts[2])
        elif parts[1] == "STATE":
            pass
        elif parts[1] == "MEDIA_SRC":
            val = int(parts[2])
            if val == 1 and not self.stream_file.get():
                self._send_cmd("SET_MEDIA_SRC:0")
            else:
                self.media_src_var.set(val)
                self.btn_src.configure(text="Wi-Fi" if val == 1 else "SD")
        elif parts[1] == "MEDIA_STATE":
            state = int(parts[2])
            self.media_state = state
            states = ["Stopped", "Playing", "Paused"]
            self.media_state_lbl.configure(text=states[state])
            if hasattr(self, 'btn_pause'):
                self.btn_pause.configure(text="Resume" if state == 2 else "Pause")
            if self.media_src_var.get() == 1:
                if state == 1 and not self.streaming:
                    self._stream()
                elif state == 0 and self.streaming:
                    self._stop_stream()

        elif parts[1] == "DIR":
            if len(parts) > 2:
                items = line.split(":", 2)[2].split("|")
                self.media_listbox.delete(0, tk.END)
                for it in items:
                    if it: self.media_listbox.insert(tk.END, it)
        elif parts[1] == "CH_VOL":
            self.ch_vars[int(parts[2])]['vol'].set(int(parts[3]))
        elif parts[1] == "CH_FREQ":
            self.ch_vars[int(parts[2])]['freq'].set(int(parts[3]))

        elif parts[1] == "CH_EN":
            idx = int(parts[2])
            nv = int(parts[3])
            self.ch_vars[idx]['en'].set(nv)
            btn = self.ch_vars[idx]['en_btn']
            btn.configure(text="Disable" if nv else "Enable", fg_color="#B22222" if nv else "#2B7A0B", hover_color="#8B0000" if nv else "#1E5C06")
        elif parts[1] == "CH_TEMPO":
            idx = int(parts[2])
            nv = int(parts[3])
            self.ch_vars[idx]['tempo_var'].set(nv)
            t_names = ["T: OFF", "T: 0.5s", "T: 1s", "T: 2s", "T: 4s"]
            self.ch_vars[idx]['tempo_btn'].configure(text=t_names[nv])
        elif parts[1].startswith("EQ_"):
            band = int(parts[1][3:])
            val = int(parts[2])
            if band == 0: self.eq_l_var.set(val)
            elif band == 1: self.eq_lm_var.set(val)
            elif band == 2: self.eq_m_var.set(val)
            elif band == 3: self.eq_hm_var.set(val)
            elif band == 4: self.eq_h_var.set(val)
        elif parts[1] == "FX_TYPE":
            idx = int(parts[2])
            fx_names = ["None", "Echo", "Revrb", "Dist", "Fuzz", "Trem", "BitC", "Decim", "Chor", "Flang", "Wah", "Ring", "Sub", "OctF", "Vibr", "Tele", "Slap", "Gate", "Phas", "Robot", "PtSh", "LoFi", "Tape", "Stut", "RvEch", "S&H", "Comb", "Formt", "Shmmr", "Radio", "WahF"]
            if 0 <= idx < len(fx_names):
                self.fx_var.set(fx_names[idx])
        elif parts[1] == "FX_EN":
            self.fx_en_var.set(int(parts[2]))
        elif parts[1] == "CH_TYPE":
            idx = int(parts[3])
            types = ["Sine", "Square", "Triangle", "Sawtooth", "Noise", "Kick", "Snare", "HiHat", "Crash", "Tom", "Clap", "Cowbell", "Ride", "Woodblk", "Bongo", "Conga", "Tambor", "Shaker", "Laser", "Bell", "RimSh", "FlrTom", "Guiro", "Maracs", "808K", "808Cl", "Timbal", "Agogo", "TriHit", "FMBel", "Siren", "ZapDn", "Metal", "PwrK", "Buzz"]
            if 0 <= idx < len(types):
                self.ch_vars[int(parts[2])]['type'].set(types[idx])
        elif parts[1] == "M_VOL":
            self.master_vol.set(int(parts[2]))
        elif parts[1] == "M_EN":
            nv = int(parts[2])
            self.master_en.set(nv)
            if hasattr(self, 'master_en_btn'):
                self.master_en_btn.configure(text="EN" if nv else "DIS", fg_color="#2B7A0B" if nv else "#B22222", hover_color="#1E5C06" if nv else "#8B0000")
        elif parts[1] == "OVERDRIVE":
            self.od_en = int(parts[2])
            if hasattr(self, 'od_btn'):
                self.od_btn.configure(text="OD: ON" if self.od_en else "OD: OFF", fg_color="#FF8000" if self.od_en else "#444444")
        elif parts[1] == "SPEED":
            self.sp_val = int(parts[2]) / 100.0
            if hasattr(self, 'sp_btn'):
                txt = "Spd: 1x" if self.sp_val == 1.0 else f"S:{self.sp_val:.2f}"
                self.sp_btn.configure(text=txt)
        elif parts[1] == "AGC_EN":
            val = int(parts[2])
            self.agc_var.set(val)
            self.agc_btn.configure(fg_color="#D16D19" if val else "#444444")
        elif parts[1] == "FX_TYPE2":
            idx = int(parts[2])
            fx_names = ["None", "Echo", "Revrb", "Dist", "Fuzz", "Trem", "BitC", "Decim", "Chor", "Flang", "Wah", "Ring", "Sub", "OctF", "Vibr", "Tele", "Slap", "Gate", "Phas", "Robot", "PtSh", "LoFi", "Tape", "Stut", "RvEch", "S&H", "Comb", "Formt", "Shmmr", "Radio", "WahF"]
            if 0 <= idx < len(fx_names):
                self.fx_var2.set(fx_names[idx])
        elif parts[1] == "FX_EN2":
            self.fx_en_var2.set(int(parts[2]))
        elif parts[1] == "BOOST":
            self.booster_var.set(int(parts[2]))

    def _toggle_overdrive(self):
        self.od_en = 1 if self.od_en == 0 else 0
        self._send_cmd(f"SET_OVERDRIVE:{self.od_en}")

    def _toggle_speed(self):
        if self.sp_val == 0.25: self.sp_val = 0.5
        elif self.sp_val == 0.5: self.sp_val = 0.75
        elif self.sp_val == 0.75: self.sp_val = 1.0
        elif self.sp_val == 1.0: self.sp_val = 1.25
        elif self.sp_val == 1.25: self.sp_val = 1.5
        elif self.sp_val == 1.5: self.sp_val = 1.75
        elif self.sp_val == 1.75: self.sp_val = 2.0
        elif self.sp_val == 2.0: self.sp_val = 0.25
        self._send_cmd(f"SET_SPEED:{int(self.sp_val * 100)}")

    def _set_esp_theme(self, choice):
        idx = list(THEMES.keys()).index(choice)
        self._send_cmd(f"SET_THEME:{idx}")

    def _confirm_reset(self):
        import tkinter.messagebox as messagebox
        if messagebox.askyesno("Reset", "Restart ESP32?"):
            self._send_cmd("RESET")

    def _send_cmd(self, cmd):
        if self.ctrl_connected and self.ctrl_sock:
            try:
                self.ctrl_sock.sendall((cmd + '\n').encode('utf-8'))
            except Exception:
                pass


    def _apply_theme(self, choice):
        t = THEMES[choice]
        
        if choice == "Matrix":
            self.matrix_canvas.place(x=0, y=0, relwidth=1, relheight=1)
            self.cyberpunk_canvas.place_forget()
        elif choice == "Cyberpunk":
            self.cyberpunk_canvas.place(x=0, y=0, relwidth=1, relheight=1)
            self.matrix_canvas.place_forget()
        else:
            self.matrix_canvas.place_forget()
            self.cyberpunk_canvas.place_forget()
            
        self.configure(fg_color=t["bg"])
        if hasattr(self, 's_ip_lbl'):
            self.s_ip_lbl.configure(fg_color=t["accent"])
        
        self.fixed_color_widgets = [self.remote_vu, self.s_ip_lbl] if hasattr(self, 's_ip_lbl') else [self.remote_vu]
        if hasattr(self, 'btn_tab_stream'):
            self.fixed_color_widgets.extend([self.btn_tab_stream, self.btn_tab_remote])

        def update_colors(widget):
            if widget in self.fixed_color_widgets:
                pass
            elif isinstance(widget, ctk.CTkButton):
                widget.configure(fg_color=t["accent"], hover_color=t["hover"])
            elif isinstance(widget, ctk.CTkSlider):
                widget.configure(progress_color=t["accent"], button_color=t["accent"], button_hover_color=t["hover"])
            elif isinstance(widget, ctk.CTkCheckBox):
                widget.configure(fg_color=t["accent"], hover_color=t["hover"])
            elif isinstance(widget, ctk.CTkOptionMenu):
                widget.configure(fg_color=t["accent"], button_color=t["accent"], button_hover_color=t["hover"], dropdown_hover_color=t["hover"])
            elif isinstance(widget, tk.Listbox):
                widget.configure(selectbackground=t["accent"])
            elif isinstance(widget, ctk.CTkProgressBar):
                widget.configure(progress_color=t["accent"])

            for child in widget.winfo_children():
                update_colors(child)
                
        update_colors(self)
        
        if hasattr(self, 'current_tab'):
            self._switch_tab(self.current_tab)

    def _browse(self):
        files = filedialog.askopenfilenames(filetypes=[("Audio Files", "*.wav *.mp3 *.flac *.ogg *.m4a *.aac *.wma *.aiff *.alac")])
        if files: 
            self.src_file.set(";".join(files))

    def _stream_browse(self):
        f = filedialog.askopenfilename(filetypes=[("WAV Files", "*_8bit.wav")])
        if f: self.stream_file.set(f)

    def _convert(self):
        if not shutil.which("ffmpeg"):
            messagebox.showerror("Error", "FFmpeg not found. Please install FFmpeg and ensure it is in your system PATH.")
            return
            
        src_str = self.src_file.get()
        if not src_str:
            messagebox.showwarning("Warning", "No file selected.")
            return
            
        files = [f.strip() for f in src_str.split(";") if f.strip() and os.path.exists(f.strip())]
        if not files:
            messagebox.showwarning("Warning", "Invalid file paths.")
            return
            
        threading.Thread(target=self._convert_worker, args=(files,), daemon=True).start()

    def _convert_worker(self, files):
        total = len(files)
        self._log(f"Starting bulk conversion ({total} files)...")
        self.convert_btn.configure(state="disabled")
        
        startupinfo = None
        if os.name == 'nt':
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            
        try:
            last_out_name = ""
            for i, src in enumerate(files):
                self._log(f"Converting [{i+1}/{total}]: {os.path.basename(src)}")
                out_name = os.path.splitext(src)[0] + "_8bit.wav"
                
                # The Holy Grail 8-Bit Audio Conversion Engine (Absolute Limit):
                # 1. aformat=mono: Downmixes to mono FIRST so dynamics processing is perfectly accurate.
                # 2. dynaudnorm: Broadcast-grade peak riding that boosts quiet background sounds perfectly.
                # 3. aresample=soxr: World's best alias-free resampling to 22050Hz.
                # 4. dither_method=triangular: TPDF dither is mathematically superior for 22050Hz 
                
                cmd = [
                    "ffmpeg", "-y", "-i", src,
                    "-af", "aformat=channel_layouts=mono,dynaudnorm=p=0.95:m=10.0,aresample=resampler=soxr:osr=22050:dither_method=triangular",
                    "-c:a", "pcm_u8",
                    out_name
                ]
                
                subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, startupinfo=startupinfo)
                self._log(f"Saved: {os.path.basename(out_name)}")
                last_out_name = out_name
            
            self._log(f"Bulk conversion complete! ({total} files)")
            if last_out_name:
                self.stream_file.set(last_out_name)
            
            self.convert_btn.configure(text="Converted!")
            time.sleep(1)
            self.convert_btn.configure(text="Convert to SD WAV (8-bit Mono)")
        except subprocess.CalledProcessError:
            self._log("Error: FFmpeg conversion failed.")
        except FileNotFoundError:
            self._log("Error: FFmpeg not found on system PATH.")
        except Exception as e:
            self._log(f"Error: {str(e)}")
        finally:
            self.convert_btn.configure(state="normal")


    def _toggle_media_src(self):
        val = 1 if self.media_src_var.get() == 0 else 0
        if val == 1 and not self.stream_file.get():
            messagebox.showwarning("Warning", "Select a _8bit.wav file to stream first.")
            return
        self.media_src_var.set(val)
        self.btn_src.configure(text="Wi-Fi" if val == 1 else "SD")
        self._send_cmd(f"SET_MEDIA_SRC:{val}")

    def _remote_play(self):
        self._send_cmd("MEDIA_PLAY")

    def _remote_pause(self):
        self._send_cmd("MEDIA_PAUSE")

    def _remote_stop(self):
        self._send_cmd("MEDIA_STOP")

    def _remote_seek(self, val):
        pct = int(val * 100)
        self._send_cmd(f"MEDIA_SEEK:{pct}")
        if self.media_src_var.get() == 1:
            self.seek_req = pct

    def _toggle_agc(self):
        val = 1 if self.agc_var.get() == 0 else 0
        self.agc_var.set(val)
        self.agc_btn.configure(fg_color="#D16D19" if val else "#444444")
        self._send_cmd(f"SET_AGC:{val}")

    def _stream(self):
        if self.streaming: return
        f = self.stream_file.get()
        ip = self.esp_ip.get()

        if not f or not os.path.exists(f):
            self._send_cmd("MEDIA_STOP")
            return
        self.streaming = True
        self.media_state = 1
        self.seek_req = -1
        self.stream_thread = threading.Thread(target=self._stream_worker, args=(f, ip), daemon=True)
        self.stream_thread.start()

    def _stop_stream(self):
        self.streaming = False
        self.media_state = 0
        if self.sock:
            try:
                self.sock.close()
            except:
                pass

    def _stream_worker(self, filepath, ip):
        self._log(f"Connecting to {ip}:{STREAM_PORT}...")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        
        try:
            self.sock.connect((ip, STREAM_PORT))
            # Disable Nagle's algorithm for low-latency streaming
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            # Increase send buffer for smoother throughput
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
            self._log("Streaming...")
            self.sock.settimeout(5.0)
            
            with wave.open(filepath, 'rb') as wf:
                framerate = wf.getframerate()
                bits = wf.getsampwidth() * 8
                channels = wf.getnchannels()
                total_frames = wf.getnframes()
                bytes_per_frame = wf.getsampwidth() * channels
                
                header = b'SMIX' + struct.pack('<I', framerate) + struct.pack('<I', bits)
                self.sock.sendall(header)
                
                chunk_frames = 4096  # frames per chunk
                sent_frames = 0
                
                # Rate-pacing: calculate how long each chunk should take
                chunk_duration = chunk_frames / framerate  # seconds per chunk
                
                while self.streaming:
                    if self.seek_req >= 0:
                        sent_frames = int((self.seek_req / 100.0) * total_frames)
                        wf.setpos(sent_frames)
                        self.seek_req = -1
                        
                    if self.media_state == 2:
                        time.sleep(0.1)
                        continue
                    elif self.media_state == 0:
                        break
                    
                    t0 = time.monotonic()
                    data = wf.readframes(chunk_frames)
                    if not data: 
                        self.media_state = 0
                        break
                        
                    try:
                        self.sock.sendall(data)
                        actual_frames = len(data) // bytes_per_frame
                        sent_frames += actual_frames
                        progress = min(1.0, sent_frames / total_frames)
                        self.after(0, self.prog_level.set, progress)
                        
                        # Rate limiting: sleep to match real-time playback
                        # Send slightly faster (0.85x) to keep ESP32 buffer fed
                        elapsed = time.monotonic() - t0
                        target = chunk_duration * 0.85
                        if elapsed < target:
                            time.sleep(target - elapsed)
                    except Exception:
                        break
                        
            if self.streaming:
                self._log("Stream Finished.")
                self.after(0, lambda: self._send_cmd("MEDIA_STOP"))
        except Exception as e:
            if self.streaming: 
                self._log(f"Stream Error: {str(e)}")
        finally:
            self.streaming = False
            self.media_state = 0
            if self.sock:
                self.sock.close()
                self.sock = None

if __name__ == "__main__":
    app = SoundControlApp()
    app.mainloop()

