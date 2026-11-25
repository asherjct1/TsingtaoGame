#!/usr/bin/env python3
"""
build_audio.py — Procedural 90s arcade SFX synthesizer + hardware-accurate PS1 SPU ADPCM (.VAG) encoder
for TSINGTAO EXPRESS '96.

Pure Python standard library only (math, struct, wave, random).
Outputs:
  - assets/gen/sfx/*.vag         (48-byte VAGp header + 16-byte aligned PS1 ADPCM payload)
  - assets/gen/sfx_table.h       (enum SfxId, SfxEntry struct, g_sfx_table declaration, budget macros)
  - assets/gen/sfx_blobs.c       (16-byte aligned raw ADPCM payload arrays ready for direct SpuWrite)
"""

import argparse
import math
import random
import struct
import sys
import wave
from pathlib import Path
from typing import List, Tuple

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT_DIR = PROJECT_ROOT / "assets" / "gen"

SAMPLE_RATE = 22050
SPU_PITCH_22050 = (SAMPLE_RATE * 4096) // 44100  # 2048
MAX_SPU_BUDGET_BYTES = 250 * 1024  # Strictly well under PS1's 512 KB SPU RAM

# Official Sony PlayStation 1 SPU ADPCM prediction filter coefficients (divided by 64)
PSX_FILTER_POS = (0, 60, 115, 98, 122)
PSX_FILTER_NEG = (0, 0, -52, -55, -60)

# SPU ADPCM block flags
VAG_FLAG_NORMAL = 0x00
VAG_FLAG_LOOP_END = 0x01
VAG_FLAG_LOOP_REPEAT = 0x02
VAG_FLAG_LOOP_START = 0x04
VAG_FLAG_SILENT_END = 0x07  # Loop Start | Loop Repeat | Loop End on silent zero block


# ---------------------------------------------------------------------------
# Basic DSP Primitives
# ---------------------------------------------------------------------------

def clamp(x: float, lo: float, hi: float) -> float:
    return lo if x < lo else (hi if x > hi else x)


def clamp16(x: int) -> int:
    return -32768 if x < -32768 else (32767 if x > 32767 else x)


def make_buf(duration_sec: float) -> List[float]:
    n = max(28, int(round(duration_sec * SAMPLE_RATE)))
    return [0.0] * n


def biquad_coeffs(ftype: str, freq: float, q: float, sr: int = SAMPLE_RATE):
    freq = clamp(freq, 20.0, sr * 0.45)
    w0 = 2.0 * math.pi * freq / sr
    cos_w0 = math.cos(w0)
    sin_w0 = math.sin(w0)
    alpha = sin_w0 / (2.0 * max(0.1, q))

    if ftype == "lp":
        b0 = (1.0 - cos_w0) * 0.5
        b1 = 1.0 - cos_w0
        b2 = (1.0 - cos_w0) * 0.5
    elif ftype == "hp":
        b0 = (1.0 + cos_w0) * 0.5
        b1 = -(1.0 + cos_w0)
        b2 = (1.0 + cos_w0) * 0.5
    elif ftype == "bp":
        b0 = alpha
        b1 = 0.0
        b2 = -alpha
    else:
        raise ValueError(f"Unknown filter type: {ftype}")

    a0 = 1.0 + alpha
    a1 = -2.0 * cos_w0
    a2 = 1.0 - alpha
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def apply_biquad(samples: List[float], ftype: str, freq: float, q: float = 0.707) -> List[float]:
    b0, b1, b2, a1, a2 = biquad_coeffs(ftype, freq, q)
    out = [0.0] * len(samples)
    x1 = x2 = y1 = y2 = 0.0
    for i, x0 in enumerate(samples):
        y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        out[i] = y0
        x2, x1 = x1, x0
        y2, y1 = y1, y0
    return out


def apply_swept_biquad(
    samples: List[float],
    ftype: str,
    freq_start: float,
    freq_end: float,
    q: float = 1.2,
    update_step: int = 32,
) -> List[float]:
    n = len(samples)
    out = [0.0] * n
    x1 = x2 = y1 = y2 = 0.0
    b0 = b1 = b2 = a1 = a2 = 0.0
    for i, x0 in enumerate(samples):
        if i % update_step == 0:
            t = i / max(1, n - 1)
            # Exponential frequency sweep sounds natural
            freq = freq_start * ((freq_end / max(1.0, freq_start)) ** t)
            b0, b1, b2, a1, a2 = biquad_coeffs(ftype, freq, q)
        y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        out[i] = y0
        x2, x1 = x1, x0
        y2, y1 = y1, y0
    return out


def add_mix(dst: List[float], src: List[float], offset_sec: float = 0.0, gain: float = 1.0) -> None:
    start = int(round(offset_sec * SAMPLE_RATE))
    end = min(len(dst), start + len(src))
    for i in range(max(0, start), end):
        dst[i] += src[i - start] * gain


def add_sine_sweep(
    buf: List[float],
    t_start: float,
    dur: float,
    f0: float,
    f1: float,
    amp: float,
    attack: float = 0.003,
    decay_curve: float = 2.0,
) -> None:
    s0 = int(round(t_start * SAMPLE_RATE))
    n = int(round(dur * SAMPLE_RATE))
    phase = 0.0
    for i in range(n):
        idx = s0 + i
        if idx < 0 or idx >= len(buf):
            continue
        t = i / max(1, n - 1)
        sec = i / SAMPLE_RATE
        freq = f0 * ((f1 / max(1.0, f0)) ** t)
        phase += 2.0 * math.pi * freq / SAMPLE_RATE
        env_a = min(1.0, sec / max(1e-5, attack))
        env_d = (1.0 - t) ** decay_curve
        buf[idx] += math.sin(phase) * amp * env_a * env_d


def add_fm_bell(
    buf: List[float],
    t_start: float,
    dur: float,
    freq: float,
    mod_ratio: float,
    mod_index: float,
    amp: float,
    decay: float = 5.0,
) -> None:
    s0 = int(round(t_start * SAMPLE_RATE))
    n = int(round(dur * SAMPLE_RATE))
    w_c = 2.0 * math.pi * freq / SAMPLE_RATE
    w_m = 2.0 * math.pi * freq * mod_ratio / SAMPLE_RATE
    for i in range(n):
        idx = s0 + i
        if idx < 0 or idx >= len(buf):
            continue
        sec = i / SAMPLE_RATE
        t = i / max(1, n - 1)
        env = math.exp(-decay * t) * min(1.0, sec / 0.002) * (1.0 - t * 0.25)
        mod_env = math.exp(-decay * 1.4 * t)
        val = math.sin(w_c * i + mod_index * mod_env * math.sin(w_m * i))
        buf[idx] += val * amp * env


def add_synth_note(
    buf: List[float],
    t_start: float,
    dur: float,
    freq: float,
    amp: float,
    wave_type: str = "brass",
) -> None:
    s0 = int(round(t_start * SAMPLE_RATE))
    n = int(round(dur * SAMPLE_RATE))
    for i in range(n):
        idx = s0 + i
        if idx < 0 or idx >= len(buf):
            continue
        sec = i / SAMPLE_RATE
        t = i / max(1, n - 1)
        # Crisp arcade ADSR
        atk = min(1.0, sec / 0.008)
        rel = min(1.0, (dur - sec) / 0.025) if dur > 0.03 else (1.0 - t)
        env = atk * max(0.0, rel) * (0.65 + 0.35 * math.exp(-5.0 * t))

        if wave_type == "brass":
            # 90s FM brassy saw-like harmonic stack (bright attack)
            bright = 0.4 + 0.6 * math.exp(-4.0 * t)
            ph1 = 2.0 * math.pi * freq * sec
            ph2 = 2.0 * math.pi * (freq * 1.004) * sec
            val = (
                0.55 * math.sin(ph1 + 1.4 * bright * math.sin(ph1))
                + 0.30 * math.sin(ph2 * 2.0) * bright
                + 0.15 * math.sin(ph1 * 3.0) * bright
            )
        elif wave_type == "square":
            ph = (freq * sec) % 1.0
            val = 1.0 if ph < 0.5 else -1.0
            val += 0.35 * math.sin(2.0 * math.pi * freq * 2.0 * sec)
        elif wave_type == "saw":
            ph1 = (freq * sec) % 1.0
            ph2 = (freq * 1.006 * sec) % 1.0
            val = ((2.0 * ph1 - 1.0) + (2.0 * ph2 - 1.0)) * 0.5
        elif wave_type == "bass":
            ph = 2.0 * math.pi * freq * sec
            val = 0.7 * math.sin(ph) + 0.3 * math.sin(2.0 * ph) * math.exp(-4.0 * t)
        else:
            val = math.sin(2.0 * math.pi * freq * sec)

        buf[idx] += val * amp * env


def finalize_pcm(buf: List[float], peak_target: float = 0.90) -> List[int]:
    # Gentle soft saturation + DC removal + peak normalization + clean fade-in/out
    n = len(buf)
    if n == 0:
        return [0] * 28

    mean = sum(buf) / n
    cleaned = [x - mean for x in buf]

    # Soft tanh limiter for punchy retro arcade warmth without harsh digital clipping
    peak = max((abs(x) for x in cleaned), default=1e-9)
    if peak < 1e-9:
        return [0] * n

    scale = peak_target / peak
    out = [math.tanh(x * scale * 1.15) / math.tanh(1.15) * peak_target for x in cleaned]

    # 1.5 ms fade-in and 6 ms fade-out so PS1 ADPCM transitions with zero click
    fade_in = min(n // 8, int(0.0015 * SAMPLE_RATE))
    fade_out = min(n // 4, int(0.006 * SAMPLE_RATE))
    for i in range(fade_in):
        out[i] *= i / max(1, fade_in)
    for i in range(fade_out):
        out[n - 1 - i] *= i / max(1, fade_out)

    # Re-normalize peak cleanly to peak_target
    p2 = max((abs(x) for x in out), default=1e-9)
    if p2 > 1e-9:
        g = peak_target / p2
        out = [x * g for x in out]

    return [clamp16(int(round(x * 32767.0))) for x in out]


# ---------------------------------------------------------------------------
# 12 Retro Beer & 90s Arcade Sound Effects (22050 Hz Mono)
# ---------------------------------------------------------------------------

def synth_can_pop() -> List[int]:
    """0: SFX_CAN_POP — crisp aluminum tab snap + fizzy psssht."""
    rng = random.Random(100)
    buf = make_buf(0.42)

    # Tab lift click (t=0.000)
    click1 = [rng.uniform(-1.0, 1.0) * math.exp(-i / 110.0) for i in range(int(0.025 * SAMPLE_RATE))]
    click1 = apply_biquad(click1, "hp", 2200.0, 1.2)
    add_mix(buf, click1, 0.0, 0.95)
    add_fm_bell(buf, 0.0, 0.035, 3150.0, 1.62, 1.2, 0.48, decay=11.0)

    # Seal break metallic snap (t=0.030)
    click2 = [rng.uniform(-1.0, 1.0) * math.exp(-i / 180.0) for i in range(int(0.04 * SAMPLE_RATE))]
    click2 = apply_biquad(click2, "bp", 3800.0, 1.8)
    add_mix(buf, click2, 0.028, 1.1)
    add_fm_bell(buf, 0.028, 0.055, 2420.0, 1.83, 1.5, 0.55, decay=9.5)
    add_sine_sweep(buf, 0.028, 0.045, 720.0, 290.0, 0.35, attack=0.001, decay_curve=2.2)

    # Carbonated fizzy psssht (t=0.032..0.41)
    fizz_n = int(0.38 * SAMPLE_RATE)
    fizz = []
    for i in range(fizz_n):
        t = i / fizz_n
        env = min(1.0, i / 180.0) * ((1.0 - t) ** 1.6)
        # Effervescent micro-crackle bursts
        crackle = 1.65 if rng.random() < 0.045 else 1.0
        fizz.append(rng.uniform(-1.0, 1.0) * env * crackle)
    fizz = apply_swept_biquad(fizz, "hp", 3600.0, 6800.0, q=0.9)
    add_mix(buf, fizz, 0.032, 0.85)

    return finalize_pcm(buf, 0.92)


def synth_bottle_slide() -> List[int]:
    """1: SFX_BOTTLE_SLIDE — smooth glass-on-wood counter slide."""
    rng = random.Random(101)
    buf = make_buf(0.35)

    # Wooden bar placement thud
    add_sine_sweep(buf, 0.0, 0.05, 185.0, 92.0, 0.55, attack=0.002, decay_curve=2.4)
    # Subtle glass bottle body ring
    add_fm_bell(buf, 0.004, 0.09, 2120.0, 1.47, 0.7, 0.25, decay=8.5)

    # Smooth varnished wood sliding friction whoosh
    slide_n = int(0.33 * SAMPLE_RATE)
    raw = []
    for i in range(slide_n):
        t = i / slide_n
        env = math.sin(math.pi * (t ** 0.7)) * ((1.0 - t) ** 0.5)
        raw.append(rng.uniform(-1.0, 1.0) * env)

    wood_band = apply_swept_biquad(raw, "bp", 920.0, 420.0, q=2.2)
    glass_band = apply_swept_biquad(raw, "bp", 2600.0, 1500.0, q=3.0)
    low_rub = apply_swept_biquad(raw, "bp", 310.0, 180.0, q=1.8)

    add_mix(buf, wood_band, 0.01, 1.15)
    add_mix(buf, glass_band, 0.01, 0.55)
    add_mix(buf, low_rub, 0.01, 0.75)

    return finalize_pcm(buf, 0.88)


def synth_bag_pour() -> List[int]:
    """2: SFX_BAG_POUR — bubbly draft pour splash into Qingdao plastic beer bag."""
    rng = random.Random(102)
    buf = make_buf(0.52)

    # Liquid pour stream noise bed
    stream_n = int(0.49 * SAMPLE_RATE)
    raw = []
    for i in range(stream_n):
        t = i / stream_n
        flutter = 0.75 + 0.25 * math.sin(2.0 * math.pi * 22.0 * (i / SAMPLE_RATE))
        env = min(1.0, i / 450.0) * ((1.0 - t) ** 0.9) * flutter
        raw.append(rng.uniform(-1.0, 1.0) * env)
    liquid = apply_swept_biquad(raw, "bp", 820.0, 1950.0, q=1.5)
    foam = apply_swept_biquad(raw, "hp", 3200.0, 5200.0, q=0.8)
    add_mix(buf, liquid, 0.01, 0.70)
    add_mix(buf, foam, 0.14, 0.35)

    # Cascade of cheerful rising liquid glug/bubbles (pitch rises as bag fills!)
    t_cur = 0.015
    for k in range(15):
        base_f = 260.0 + k * 34.0 + rng.uniform(-25.0, 35.0)
        end_f = base_f * rng.uniform(1.65, 2.25)
        dur = rng.uniform(0.026, 0.044)
        amp = rng.uniform(0.28, 0.44) * (1.0 - 0.3 * (k / 15.0))
        add_sine_sweep(buf, t_cur, dur, base_f, end_f, amp, attack=0.002, decay_curve=1.3)
        t_cur += rng.uniform(0.022, 0.036)

    return finalize_pcm(buf, 0.90)


def synth_drink_catch() -> List[int]:
    """3: SFX_DRINK_CATCH — satisfied gulp + cheerful clink."""
    buf = make_buf(0.36)

    # Bright glass clink
    add_fm_bell(buf, 0.0, 0.14, 2380.0, 1.56, 0.9, 0.45, decay=9.0)
    add_fm_bell(buf, 0.0, 0.10, 3720.0, 1.21, 0.6, 0.32, decay=11.0)

    # Satisfied cartoon "gulp!"
    add_sine_sweep(buf, 0.05, 0.075, 240.0, 560.0, 0.55, attack=0.004, decay_curve=1.2)
    add_sine_sweep(buf, 0.11, 0.070, 490.0, 290.0, 0.42, attack=0.004, decay_curve=1.6)

    # Cheerful 2-note arcade reward chime (C6 -> G6)
    add_fm_bell(buf, 0.16, 0.11, 1046.5, 2.0, 0.9, 0.45, decay=6.0)
    add_fm_bell(buf, 0.22, 0.14, 1568.0, 2.0, 1.0, 0.52, decay=5.5)

    return finalize_pcm(buf, 0.92)


def synth_empty_catch() -> List[int]:
    """4: SFX_EMPTY_CATCH — snappy catch pop."""
    rng = random.Random(104)
    buf = make_buf(0.22)

    # Snappy upward pop
    add_sine_sweep(buf, 0.0, 0.055, 230.0, 860.0, 0.65, attack=0.001, decay_curve=1.8)
    # Crisp hand-slap/can-tap transient
    tap = [rng.uniform(-1.0, 1.0) * math.exp(-i / 90.0) for i in range(int(0.02 * SAMPLE_RATE))]
    tap = apply_biquad(tap, "bp", 1900.0, 1.6)
    add_mix(buf, tap, 0.0, 0.75)
    # Short bright arcade blip (E6)
    add_fm_bell(buf, 0.035, 0.15, 1318.5, 2.0, 0.7, 0.48, decay=7.5)

    return finalize_pcm(buf, 0.92)


def synth_glass_smash() -> List[int]:
    """5: SFX_GLASS_SMASH — crunchy glass shatter crash."""
    rng = random.Random(105)
    buf = make_buf(0.62)

    # Heavy floor impact thump
    add_sine_sweep(buf, 0.0, 0.09, 145.0, 38.0, 0.75, attack=0.001, decay_curve=2.4)

    # Wideband initial crash noise
    crash_n = int(0.32 * SAMPLE_RATE)
    crash = [rng.uniform(-1.0, 1.0) * ((1.0 - i / crash_n) ** 2.6) for i in range(crash_n)]
    crash_hi = apply_biquad(crash, "hp", 1800.0, 0.8)
    crash_mid = apply_biquad(crash, "bp", 950.0, 1.0)
    add_mix(buf, crash_hi, 0.0, 1.05)
    add_mix(buf, crash_mid, 0.0, 0.65)

    # 28 sparkling bouncing glass shards (staggered over 0..0.44s)
    for k in range(28):
        t_hit = (k / 28.0) ** 1.7 * 0.42 + rng.uniform(0.0, 0.015)
        freq = rng.uniform(2100.0, 7800.0)
        dur = rng.uniform(0.045, 0.14)
        amp = rng.uniform(0.14, 0.34) * (1.0 - 0.55 * (t_hit / 0.45))
        add_fm_bell(buf, t_hit, dur, freq, rng.uniform(1.31, 1.89), 1.4, amp, decay=10.5)

    return finalize_pcm(buf, 0.94)


def synth_tip_pickup() -> List[int]:
    """6: SFX_TIP_PICKUP — bright arcade ka-ching!"""
    rng = random.Random(106)
    buf = make_buf(0.48)

    # Mechanical register "ka-" click + first coin tone (B5)
    snap = [rng.uniform(-1.0, 1.0) * math.exp(-i / 100.0) for i in range(int(0.022 * SAMPLE_RATE))]
    snap = apply_biquad(snap, "hp", 2600.0, 1.1)
    add_mix(buf, snap, 0.0, 0.75)
    add_fm_bell(buf, 0.0, 0.09, 987.77, 2.0, 1.1, 0.55, decay=8.0)

    # Sparkling "-CHING!" (E6 + metallic overtone shimmer)
    add_fm_bell(buf, 0.072, 0.40, 1318.51, 2.0, 1.4, 0.65, decay=4.6)
    add_fm_bell(buf, 0.072, 0.36, 2637.02, 1.5, 1.1, 0.38, decay=5.4)
    add_fm_bell(buf, 0.076, 0.28, 3955.53, 2.41, 0.9, 0.24, decay=6.8)
    add_fm_bell(buf, 0.080, 0.22, 5274.04, 1.33, 0.6, 0.16, decay=8.0)

    return finalize_pcm(buf, 0.92)


def synth_rush_ready() -> List[int]:
    """7: SFX_RUSH_READY — icy ascending shimmer chime."""
    rng = random.Random(107)
    buf = make_buf(0.70)

    # Sparkling major-9th ascending ice arpeggio: C5, E5, G5, B5, D6, G6, C7
    notes = [523.25, 659.25, 783.99, 987.77, 1174.66, 1567.98, 2093.00]
    for idx, freq in enumerate(notes):
        t0 = idx * 0.058
        dur = 0.28 if idx < len(notes) - 1 else 0.34
        add_fm_bell(buf, t0, dur, freq, 3.0, 1.3, 0.36, decay=5.5)
        add_fm_bell(buf, t0 + 0.008, dur * 0.85, freq * 2.005, 1.5, 0.7, 0.16, decay=6.5)

    # Icy frost crystal high-frequency shimmer bed
    ice_n = int(0.66 * SAMPLE_RATE)
    raw = [rng.uniform(-1.0, 1.0) * math.sin(math.pi * (i / ice_n)) for i in range(ice_n)]
    ice = apply_swept_biquad(raw, "bp", 3200.0, 7600.0, q=2.5)
    add_mix(buf, ice, 0.02, 0.35)

    return finalize_pcm(buf, 0.92)


def synth_rush_fire() -> List[int]:
    """8: SFX_RUSH_FIRE — huge 90s frosty synth wave blast."""
    rng = random.Random(108)
    buf = make_buf(1.05)

    # Sub-bass arcade super impact
    add_sine_sweep(buf, 0.0, 0.28, 190.0, 38.0, 0.85, attack=0.002, decay_curve=1.8)

    # Frosty wind + beer foam wave roar
    wave_n = int(1.00 * SAMPLE_RATE)
    raw = []
    for i in range(wave_n):
        t = i / wave_n
        env = min(1.0, i / 250.0) * ((1.0 - t) ** 1.15)
        raw.append(rng.uniform(-1.0, 1.0) * env)
    whoosh_up = apply_swept_biquad(raw[: int(0.45 * SAMPLE_RATE)], "bp", 450.0, 4800.0, q=1.6)
    whoosh_dn = apply_swept_biquad(raw, "bp", 3600.0, 700.0, q=1.3)
    add_mix(buf, whoosh_up, 0.0, 0.85)
    add_mix(buf, whoosh_dn, 0.05, 0.75)

    # Heroic 90s FM brass power chord blast (C4, G4, C5, E5, G5, C6)
    chord = [261.63, 392.00, 523.25, 659.25, 783.99, 1046.50]
    for idx, freq in enumerate(chord):
        add_synth_note(buf, 0.02 + idx * 0.018, 0.88 - idx * 0.03, freq, 0.22, wave_type="brass")

    # Rising frosty laser/shimmer accent
    add_sine_sweep(buf, 0.0, 0.35, 350.0, 2400.0, 0.28, attack=0.004, decay_curve=1.5)
    add_fm_bell(buf, 0.14, 0.65, 2093.0, 2.0, 1.5, 0.25, decay=4.0)

    return finalize_pcm(buf, 0.94)


def synth_wrong_drink() -> List[int]:
    """9: SFX_WRONG_DRINK — comic low arcade buzzer."""
    buf = make_buf(0.42)

    # Two punchy game-show buzz pulses ("BZZ-BZZZZ!")
    pulses = [
        (0.00, 0.13, 148.0, 140.0),
        (0.16, 0.24, 132.0, 96.0),
    ]
    for t0, dur, f0, f1 in pulses:
        s0 = int(round(t0 * SAMPLE_RATE))
        n = int(round(dur * SAMPLE_RATE))
        ph1 = 0.0
        ph2 = 0.0
        for i in range(n):
            idx = s0 + i
            if idx >= len(buf):
                break
            t = i / max(1, n - 1)
            freq = f0 + (f1 - f0) * t
            ph1 = (ph1 + freq / SAMPLE_RATE) % 1.0
            # Minor-second dissonance for authentic comic buzzer crunch
            ph2 = (ph2 + (freq * 1.062) / SAMPLE_RATE) % 1.0
            saw1 = 2.0 * ph1 - 1.0
            sq2 = 1.0 if ph2 < 0.5 else -1.0
            env = min(1.0, i / 70.0) * min(1.0, (n - i) / 220.0)
            buf[idx] += (0.60 * saw1 + 0.40 * sq2) * 0.65 * env

    # Mild low-pass to round off harsh aliasing while keeping buzz crunchy
    buf = apply_biquad(buf, "lp", 2600.0, 0.9)
    return finalize_pcm(buf, 0.90)


def synth_bonus_win() -> List[int]:
    """10: SFX_BONUS_WIN — upbeat 90s promotional arcade fanfare jingle."""
    buf = make_buf(1.35)

    # Upbeat C-major 90s TV commercial / arcade victory fanfare:
    # C5 -> E5 -> G5 -> C6 -> (pickup) G5 -> C6! (with harmony + punchy bass + sparkle)
    melody = [
        (0.00, 0.10, 523.25, 0.42),  # C5
        (0.10, 0.10, 659.25, 0.44),  # E5
        (0.20, 0.10, 783.99, 0.46),  # G5
        (0.30, 0.22, 1046.50, 0.52), # C6
        (0.54, 0.11, 783.99, 0.46),  # G5
        (0.66, 0.62, 1046.50, 0.56), # C6 (triumphant hold!)
    ]
    for t0, dur, freq, amp in melody:
        add_synth_note(buf, t0, dur, freq, amp, wave_type="brass")
        add_fm_bell(buf, t0, dur + 0.08, freq * 2.0, 2.0, 0.8, amp * 0.28, decay=5.5)

    # Harmony chord hits on beats
    add_synth_note(buf, 0.30, 0.22, 659.25, 0.26, wave_type="brass")  # E5
    add_synth_note(buf, 0.30, 0.22, 783.99, 0.26, wave_type="brass")  # G5

    # Final celebratory C-major-9 chord bloom
    for freq, amp in [(523.25, 0.26), (659.25, 0.28), (783.99, 0.28), (1174.66, 0.18)]:
        add_synth_note(buf, 0.66, 0.62, freq, amp, wave_type="brass")

    # Bouncy 90s arcade bassline
    bass = [
        (0.00, 0.18, 130.81, 0.45),  # C3
        (0.20, 0.10, 196.00, 0.42),  # G3
        (0.30, 0.20, 130.81, 0.48),  # C3
        (0.54, 0.11, 196.00, 0.44),  # G3
        (0.66, 0.58, 130.81, 0.50),  # C3
    ]
    for t0, dur, freq, amp in bass:
        add_synth_note(buf, t0, dur, freq, amp, wave_type="bass")

    # Victory sparkle tail
    for idx, freq in enumerate([1318.51, 1567.98, 2093.00, 2637.02]):
        add_fm_bell(buf, 0.72 + idx * 0.055, 0.32, freq, 2.0, 1.0, 0.20, decay=5.5)

    return finalize_pcm(buf, 0.92)


def synth_foam_spray() -> List[int]:
    """11: SFX_FOAM_SPRAY — pressurized fizzy beer spray comedy gag."""
    rng = random.Random(111)
    buf = make_buf(0.95)

    # Explosive shaken-can tab POP!
    pop = [rng.uniform(-1.0, 1.0) * math.exp(-i / 140.0) for i in range(int(0.04 * SAMPLE_RATE))]
    pop = apply_biquad(pop, "hp", 1800.0, 1.0)
    add_mix(buf, pop, 0.0, 1.2)
    add_sine_sweep(buf, 0.0, 0.05, 580.0, 140.0, 0.65, attack=0.001, decay_curve=2.0)

    # Pressurized sputtering beer-foam jet ("PSSSSSHHHH-SPLORCH!")
    jet_n = int(0.90 * SAMPLE_RATE)
    raw = []
    for i in range(jet_n):
        t = i / jet_n
        sec = i / SAMPLE_RATE
        # Turbulent sputtering LFO that slows down as pressure runs out
        lfo_rate = 26.0 - 15.0 * t
        sputter = 0.62 + 0.38 * math.sin(2.0 * math.pi * lfo_rate * sec)
        env = min(1.0, i / 140.0) * ((1.0 - t) ** 0.75) * sputter
        raw.append(rng.uniform(-1.0, 1.0) * env)

    hiss = apply_swept_biquad(raw, "hp", 2800.0, 5600.0, q=0.9)
    wet_body = apply_swept_biquad(raw, "bp", 1600.0, 650.0, q=1.5)
    add_mix(buf, hiss, 0.025, 0.85)
    add_mix(buf, wet_body, 0.025, 0.78)

    # Wet comic foamy splutter bubbles throughout the spray
    t_cur = 0.06
    for _ in range(18):
        f0 = rng.uniform(220.0, 620.0)
        f1 = f0 * rng.uniform(0.55, 1.75)
        add_sine_sweep(buf, t_cur, rng.uniform(0.025, 0.05), f0, f1, 0.24, attack=0.002, decay_curve=1.5)
        t_cur += rng.uniform(0.032, 0.058)

    # Subtle comic deflating pressure drop at the tail
    add_sine_sweep(buf, 0.62, 0.28, 680.0, 180.0, 0.22, attack=0.01, decay_curve=1.6)

    return finalize_pcm(buf, 0.92)


def synth_card_flip() -> List[int]:
    """12: SFX_CARD_FLIP — crisp Balatro card snap + satin flick swish."""
    rng = random.Random(112)
    buf = make_buf(0.18)

    # Crisp card-stock flick snap
    snap = [rng.uniform(-1.0, 1.0) * math.exp(-i / 65.0) for i in range(int(0.018 * SAMPLE_RATE))]
    snap = apply_biquad(snap, "bp", 2400.0, 1.5)
    add_mix(buf, snap, 0.0, 1.1)

    # Smooth satin air swish
    swish_n = int(0.11 * SAMPLE_RATE)
    swish = [
        rng.uniform(-1.0, 1.0) * math.sin(math.pi * (i / swish_n)) * ((1.0 - i / swish_n) ** 0.7)
        for i in range(swish_n)
    ]
    swish = apply_swept_biquad(swish, "bp", 1500.0, 4600.0, q=1.4)
    add_mix(buf, swish, 0.008, 0.85)

    # Subtle bright card-turn chime (G6)
    add_sine_sweep(buf, 0.015, 0.065, 520.0, 1180.0, 0.30, attack=0.002, decay_curve=1.8)
    add_fm_bell(buf, 0.025, 0.13, 1567.98, 2.0, 0.7, 0.28, decay=7.5)

    return finalize_pcm(buf, 0.90)


def synth_shop_buy() -> List[int]:
    """13: SFX_SHOP_BUY — cheerful night-market purchase cha-ching + confirm arpeggio."""
    rng = random.Random(113)
    buf = make_buf(0.42)

    # Crisp register mechanical click
    snap = [rng.uniform(-1.0, 1.0) * math.exp(-i / 85.0) for i in range(int(0.020 * SAMPLE_RATE))]
    snap = apply_biquad(snap, "hp", 2800.0, 1.1)
    add_mix(buf, snap, 0.0, 0.8)

    # Upbeat 3-note arcade shop confirm (G5 -> B5 -> D6 -> G6)
    notes = [
        (0.000, 0.08, 783.99, 0.42),
        (0.055, 0.08, 987.77, 0.44),
        (0.110, 0.09, 1174.66, 0.46),
        (0.165, 0.24, 1567.98, 0.56),
    ]
    for t0, dur, freq, amp in notes:
        add_fm_bell(buf, t0, dur, freq, 2.0, 1.0, amp, decay=5.5)
        add_fm_bell(buf, t0 + 0.006, dur * 0.8, freq * 2.0, 1.5, 0.6, amp * 0.30, decay=6.5)

    return finalize_pcm(buf, 0.92)


def synth_mult_ping() -> List[int]:
    """14: SFX_MULT_PING — crisp Balatro-style Coaster trigger crystal ping (pitch-scalable!)."""
    buf = make_buf(0.24)

    # Snappy upward percussive pop transient
    add_sine_sweep(buf, 0.0, 0.032, 420.0, 1046.5, 0.36, attack=0.001, decay_curve=2.2)
    # Clean pitched C6 crystal bell + octave shimmer
    add_fm_bell(buf, 0.004, 0.22, 1046.50, 2.0, 0.85, 0.62, decay=6.2)
    add_fm_bell(buf, 0.008, 0.16, 2093.00, 1.5, 0.50, 0.26, decay=7.5)

    return finalize_pcm(buf, 0.92)


def synth_boss_siren() -> List[int]:
    """15: SFX_BOSS_SIREN — 90s arcade Boss Shift warning klaxon sting."""
    rng = random.Random(115)
    buf = make_buf(0.84)

    # Low arcade sub-impact
    add_sine_sweep(buf, 0.0, 0.18, 160.0, 48.0, 0.68, attack=0.002, decay_curve=1.7)
    add_sine_sweep(buf, 0.40, 0.18, 160.0, 48.0, 0.62, attack=0.002, decay_curve=1.7)

    # Two urgent FM siren horn sweeps
    for idx, (t0, f_lo, f_peak, f_end) in enumerate([
        (0.00, 392.0, 880.0, 523.25),
        (0.40, 440.0, 987.77, 587.33),
    ]):
        dur = 0.38
        s0 = int(round(t0 * SAMPLE_RATE))
        n = int(round(dur * SAMPLE_RATE))
        ph = 0.0
        for i in range(n):
            pos = s0 + i
            if pos >= len(buf):
                break
            t = i / max(1, n - 1)
            if t < 0.45:
                freq = f_lo + (f_peak - f_lo) * (t / 0.45)
            else:
                freq = f_peak + (f_end - f_peak) * ((t - 0.45) / 0.55)
            ph += 2.0 * math.pi * freq / SAMPLE_RATE
            env = min(1.0, i / 110.0) * ((1.0 - t) ** 0.45)
            # Brassy FM klaxon harmonic bite
            val = 0.62 * math.sin(ph + 1.35 * math.sin(ph)) + 0.28 * math.sin(2.0 * ph)
            buf[pos] += val * 0.58 * env

    # Crisp cymbal/metallic alert sizzle
    shimmer_n = int(0.32 * SAMPLE_RATE)
    raw = [rng.uniform(-1.0, 1.0) * ((1.0 - i / shimmer_n) ** 2.0) for i in range(shimmer_n)]
    shimmer = apply_biquad(raw, "hp", 3400.0, 1.0)
    add_mix(buf, shimmer, 0.0, 0.35)
    add_mix(buf, shimmer, 0.40, 0.35)

    return finalize_pcm(buf, 0.92)


def synth_bell_close() -> List[int]:
    """16: SFX_BELL_CLOSE — heavy brass bar Closing Time bell chime."""
    rng = random.Random(116)
    buf = make_buf(0.85)

    for strike_t, amp_scale in ((0.00, 0.85), (0.19, 1.00)):
        # Crisp metal clapper transient
        hit = [rng.uniform(-1.0, 1.0) * math.exp(-i / 65.0) for i in range(int(0.02 * SAMPLE_RATE))]
        hit = apply_biquad(hit, "bp", 3200.0, 1.8)
        add_mix(buf, hit, strike_t, 0.70 * amp_scale)

        # Rich brass bell overtone stack (A5 fundamental + metallic partials)
        dur = 0.64 if strike_t > 0.1 else 0.24
        add_fm_bell(buf, strike_t, dur, 880.00, 2.0, 1.1, 0.52 * amp_scale, decay=4.2)
        add_fm_bell(buf, strike_t, dur * 0.9, 1760.00, 1.41, 0.9, 0.34 * amp_scale, decay=5.2)
        add_fm_bell(buf, strike_t, dur * 0.75, 2489.02, 1.73, 0.7, 0.22 * amp_scale, decay=6.5)
        add_fm_bell(buf, strike_t, dur * 0.60, 3520.00, 1.25, 0.5, 0.14 * amp_scale, decay=8.0)

    return finalize_pcm(buf, 0.92)


def synth_trick_use() -> List[int]:
    """17: SFX_TRICK_USE — snappy Pocket Trick activation power-up flourish."""
    rng = random.Random(117)
    buf = make_buf(0.42)

    # Fast upward cyber/magic whoosh
    whoosh_n = int(0.18 * SAMPLE_RATE)
    raw = [rng.uniform(-1.0, 1.0) * math.sin(math.pi * (i / whoosh_n)) for i in range(whoosh_n)]
    whoosh = apply_swept_biquad(raw, "bp", 600.0, 4800.0, q=1.8)
    add_mix(buf, whoosh, 0.0, 0.85)
    add_sine_sweep(buf, 0.0, 0.15, 320.0, 1920.0, 0.38, attack=0.003, decay_curve=1.5)

    # Sparkly E-major-9 power-up chime (E5 -> G#5 -> B5 -> F#6)
    for idx, freq in enumerate([659.25, 830.61, 987.77, 1479.98]):
        t0 = 0.03 + idx * 0.042
        add_fm_bell(buf, t0, 0.24, freq, 2.0, 1.1, 0.38, decay=5.2)
        add_synth_note(buf, t0, 0.16, freq, 0.18, wave_type="brass")

    return finalize_pcm(buf, 0.92)


SFX_SPECS = [
    ("SFX_CAN_POP",      "sfx_can_pop",      synth_can_pop),
    ("SFX_BOTTLE_SLIDE", "sfx_bottle_slide", synth_bottle_slide),
    ("SFX_BAG_POUR",     "sfx_bag_pour",     synth_bag_pour),
    ("SFX_DRINK_CATCH",  "sfx_drink_catch",  synth_drink_catch),
    ("SFX_EMPTY_CATCH",  "sfx_empty_catch",  synth_empty_catch),
    ("SFX_GLASS_SMASH",  "sfx_glass_smash",  synth_glass_smash),
    ("SFX_TIP_PICKUP",   "sfx_tip_pickup",   synth_tip_pickup),
    ("SFX_RUSH_READY",   "sfx_rush_ready",   synth_rush_ready),
    ("SFX_RUSH_FIRE",    "sfx_rush_fire",    synth_rush_fire),
    ("SFX_WRONG_DRINK",  "sfx_wrong_drink",  synth_wrong_drink),
    ("SFX_BONUS_WIN",    "sfx_bonus_win",    synth_bonus_win),
    ("SFX_FOAM_SPRAY",   "sfx_foam_spray",   synth_foam_spray),
    ("SFX_CARD_FLIP",    "sfx_card_flip",    synth_card_flip),
    ("SFX_SHOP_BUY",     "sfx_shop_buy",     synth_shop_buy),
    ("SFX_MULT_PING",    "sfx_mult_ping",    synth_mult_ping),
    ("SFX_BOSS_SIREN",   "sfx_boss_siren",   synth_boss_siren),
    ("SFX_BELL_CLOSE",   "sfx_bell_close",   synth_bell_close),
    ("SFX_TRICK_USE",    "sfx_trick_use",    synth_trick_use),
]


# ---------------------------------------------------------------------------
# Hardware-Accurate Sony PS1 SPU ADPCM (.VAG) Encoder & Verifier Decoder
# ---------------------------------------------------------------------------

def encode_psx_adpcm_one_shot(pcm: List[int]) -> bytes:
    """
    Encode 16-bit mono PCM samples into hardware-accurate PS1 SPU ADPCM blocks.

    Structure (guaranteed click-free one-shot playback on real PS1 SPU):
      - Block 0: 16 zero bytes (flag 0x00) to clear SPU IIR filter history cleanly
      - Audio blocks: 28 samples -> 16 bytes each, closed-loop optimal filter (0..4) & shift (0..12), flag 0x00
      - Trailing block: 16-byte silent end block (0x00, 0x07, 0x00..0x00) setting Loop Start|Repeat|End
        so the SPU sets ENDX and loops silently on zero samples without ringing or jumping to stale RAM.
    """
    # Pad PCM length to a multiple of 28 samples with silence
    rem = len(pcm) % 28
    if rem != 0:
        padded = pcm + [0] * (28 - rem)
    else:
        padded = list(pcm)

    out = bytearray()

    # Leading 16-byte silent block (matches Sony wav2vag convention; resets old=older=0)
    out.extend(b"\x00" * 16)

    state_old = 0
    state_older = 0

    for offset in range(0, len(padded), 28):
        block = padded[offset : offset + 28]

        best_err = None
        best_filt = 0
        best_shift = 0
        best_nibbles = None
        best_old = 0
        best_older = 0

        for filt in range(5):
            f0 = PSX_FILTER_POS[filt]
            f1 = PSX_FILTER_NEG[filt]

            # Estimate max residual magnitude to pick candidate shifts quickly
            est_old = state_old
            est_older = state_older
            max_abs_res = 0
            for target in block:
                pred = (est_old * f0 + est_older * f1 + 32) >> 6
                res = abs(target - pred)
                if res > max_abs_res:
                    max_abs_res = res
                est_older = est_old
                est_old = target

            # Compute ideal shift where max_abs_res fits in [-8, +7] * (1 << (12 - shift))
            # Find largest shift in 0..12 such that (7 << (12 - shift)) >= max_abs_res
            ideal_shift = 0
            for s in range(12, -1, -1):
                if (7 << (12 - s)) >= max_abs_res:
                    ideal_shift = s
                    break

            # Test neighborhood around ideal_shift (closed-loop exact hardware reconstruction)
            cand_shifts = sorted({max(0, min(12, ideal_shift + d)) for d in (-2, -1, 0, 1, 2)})

            for shift in cand_shifts:
                rshift = 12 - shift
                half_step = (1 << (rshift - 1)) if rshift > 0 else 0

                old = state_old
                older = state_older
                err = 0
                nibs = [0] * 28

                for i, target in enumerate(block):
                    pred = (old * f0 + older * f1 + 32) >> 6
                    diff = target - pred

                    if rshift == 0:
                        nib = diff
                    elif diff >= 0:
                        nib = (diff + half_step) >> rshift
                    else:
                        nib = -((-diff + half_step) >> rshift)

                    if nib > 7:
                        nib = 7
                    elif nib < -8:
                        nib = -8

                    # Exact PS1 SPU hardware decoding step
                    recon = pred + ((nib << 12) >> shift)
                    if recon > 32767:
                        recon = 32767
                    elif recon < -32768:
                        recon = -32768

                    d = target - recon
                    err += d * d
                    older = old
                    old = recon
                    nibs[i] = nib & 0x0F

                    if best_err is not None and err >= best_err:
                        break

                if best_err is None or err < best_err:
                    best_err = err
                    best_filt = filt
                    best_shift = shift
                    best_nibbles = nibs
                    best_old = old
                    best_older = older

        state_old = best_old
        state_older = best_older

        out.append(((best_filt & 0x0F) << 4) | (best_shift & 0x0F))
        out.append(VAG_FLAG_NORMAL)
        for i in range(0, 28, 2):
            # Low nibble = first sample, High nibble = second sample
            out.append((best_nibbles[i] & 0x0F) | ((best_nibbles[i + 1] & 0x0F) << 4))

    # Pad with 0..3 silent 16-byte blocks before the final 0x07 silent-end block
    # so total payload size is strictly a multiple of 64 bytes (optimal for PS1 SPU DMA!)
    pad_zero_blocks = ((48 - (len(out) % 64)) % 64) // 16
    for _ in range(pad_zero_blocks):
        out.extend(b"\x00" * 16)

    # Trailing silent end block (filter=0, shift=0, flag=0x07, all-zero nibbles)
    out.append(0x00)
    out.append(VAG_FLAG_SILENT_END)
    out.extend(b"\x00" * 14)

    return bytes(out)


def decode_psx_adpcm(payload: bytes) -> List[int]:
    """Exact hardware PS1 SPU ADPCM decoder for self-verification & SNR calculation."""
    if len(payload) % 16 != 0:
        raise ValueError(f"ADPCM payload length {len(payload)} is not a multiple of 16 bytes")

    out: List[int] = []
    old = 0
    older = 0

    for off in range(0, len(payload), 16):
        hdr = payload[off]
        shift = hdr & 0x0F
        filt = (hdr >> 4) & 0x0F
        if shift > 12 or filt > 4:
            raise ValueError(f"Invalid ADPCM header byte 0x{hdr:02X} at offset {off}")
        f0 = PSX_FILTER_POS[filt]
        f1 = PSX_FILTER_NEG[filt]

        for b in payload[off + 2 : off + 16]:
            for raw_nib in (b & 0x0F, (b >> 4) & 0x0F):
                nib = raw_nib - 16 if raw_nib >= 8 else raw_nib
                delta = (nib << 12) >> shift
                pred = (old * f0 + older * f1 + 32) >> 6
                sample = clamp16(pred + delta)
                out.append(sample)
                older = old
                old = sample

    return out


def compute_snr_db(orig: List[int], payload: bytes) -> float:
    decoded = decode_psx_adpcm(payload)
    # Skip the leading 28-sample silent block
    dec_audio = decoded[28 : 28 + len(orig)]
    sig_pow = sum(x * x for x in orig)
    err_pow = sum((a - b) * (a - b) for a, b in zip(orig, dec_audio))
    if err_pow == 0:
        return 99.9
    if sig_pow == 0:
        return 0.0
    return 10.0 * math.log10(sig_pow / err_pow)


def pack_vag_file(payload: bytes, name: str, sample_rate: int = SAMPLE_RATE) -> bytes:
    """Wrap raw ADPCM payload in a standard 48-byte big-endian Sony VAGp v3 header."""
    name_bytes = name.encode("ascii", errors="ignore")[:16].ljust(16, b"\x00")
    header = struct.pack(
        ">4sIIII12s16s",
        b"VAGp",
        3,              # version 3
        0,              # reserved
        len(payload),   # ADPCM payload bytes
        sample_rate,    # Hz
        b"\x00" * 12,   # reserved
        name_bytes,
    )
    assert len(header) == 48
    return header + payload


def write_wav_preview(path: Path, pcm: List[int], sample_rate: int = SAMPLE_RATE) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))


# ---------------------------------------------------------------------------
# C Header & Blob Generator
# ---------------------------------------------------------------------------

def generate_c_files(
    out_dir: Path,
    entries: List[Tuple[str, str, bytes]],
) -> None:
    total_bytes = sum(len(payload) for _, _, payload in entries)

    # 1. assets/gen/sfx_table.h
    h_lines = [
        "/* Auto-generated by tools/build_audio.py — DO NOT EDIT BY HAND. */",
        "#ifndef SFX_TABLE_H",
        "#define SFX_TABLE_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define SFX_SAMPLE_RATE      {SAMPLE_RATE}u",
        f"#define SFX_DEFAULT_PITCH    {SPU_PITCH_22050}u  /* ({SAMPLE_RATE} * 4096) / 44100 */",
        f"#define SFX_TOTAL_SPU_BYTES  {total_bytes}u",
        "",
        "typedef enum SfxId {",
    ]
    for idx, (enum_name, _, _) in enumerate(entries):
        h_lines.append(f"    {enum_name:<18s} = {idx},")
    h_lines.extend([
        f"    {'SFX_COUNT':<18s} = {len(entries)}",
        "} SfxId;",
        "",
        "typedef struct SfxEntry {",
        "    const uint8_t *data;   /* 16-byte aligned PS1 SPU ADPCM payload (no 48-byte VAG header) */",
        "    uint32_t       size;   /* Payload byte length (strict multiple of 64 for SPU DMA & 16 for ADPCM) */",
        "    uint16_t       pitch;  /* PS1 SPU pitch register value (2048 = 22050 Hz) */",
        "} SfxEntry;",
        "",
        "extern const SfxEntry g_sfx_table[SFX_COUNT];",
        "",
        "#endif /* SFX_TABLE_H */",
        "",
    ])
    (out_dir / "sfx_table.h").write_text("\n".join(h_lines), encoding="utf-8")

    # 2. assets/gen/sfx_blobs.c
    c_lines = [
        "/* Auto-generated by tools/build_audio.py — DO NOT EDIT BY HAND. */",
        '#include "sfx_table.h"',
        "",
    ]

    for enum_name, file_stem, payload in entries:
        c_lines.append(
            f"/* {enum_name}: {len(payload)} bytes ({len(payload) // 16} ADPCM blocks, {SAMPLE_RATE} Hz) */"
        )
        c_lines.append(
            f"static const uint8_t s_{file_stem}_adpcm[{len(payload)}] __attribute__((aligned(16))) = {{"
        )
        for off in range(0, len(payload), 16):
            chunk = payload[off : off + 16]
            hex_row = ", ".join(f"0x{b:02X}" for b in chunk)
            comma = "," if (off + 16) < len(payload) else ""
            c_lines.append(f"    {hex_row}{comma}")
        c_lines.append("};")
        c_lines.append("")

    c_lines.append("const SfxEntry g_sfx_table[SFX_COUNT] = {")
    for idx, (enum_name, file_stem, payload) in enumerate(entries):
        comma = "," if idx + 1 < len(entries) else ""
        c_lines.append(
            f"    /* {idx:2d} {enum_name:<16s} */ {{ s_{file_stem}_adpcm, {len(payload)}u, {SPU_PITCH_22050}u }}{comma}"
        )
    c_lines.append("};")
    c_lines.append("")

    (out_dir / "sfx_blobs.c").write_text("\n".join(c_lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Synthesize and encode PS1 SPU ADPCM SFX for TSINGTAO EXPRESS '96.")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR, help="Output base directory (default: assets/gen).")
    parser.add_argument("--dump-wav", type=Path, default=None, help="Optional directory to also dump 16-bit WAV previews.")
    args = parser.parse_args()

    out_dir: Path = args.out_dir.resolve()
    sfx_dir = out_dir / "sfx"
    sfx_dir.mkdir(parents=True, exist_ok=True)

    entries: List[Tuple[str, str, bytes]] = []
    total_spu_bytes = 0
    total_duration = 0.0

    print(f"[build_audio] Generating {len(SFX_SPECS)} PS1 SPU ADPCM SFX at {SAMPLE_RATE} Hz (pitch={SPU_PITCH_22050})...")
    for idx, (enum_name, file_stem, synth_fn) in enumerate(SFX_SPECS):
        pcm = synth_fn()
        payload = encode_psx_adpcm_one_shot(pcm)

        # Verify strict hardware invariants
        assert len(payload) % 16 == 0, f"{enum_name}: payload not 16-byte aligned"
        assert len(payload) % 64 == 0, f"{enum_name}: payload not 64-byte DMA aligned"
        assert payload[0:16] == b"\x00" * 16, f"{enum_name}: missing leading silent block"
        assert payload[-16] == 0x00 and payload[-15] == VAG_FLAG_SILENT_END and payload[-14:] == b"\x00" * 14, (
            f"{enum_name}: missing trailing silent 0x07 end block"
        )
        snr = compute_snr_db(pcm, payload)
        if snr < 18.0:
            raise RuntimeError(f"{enum_name}: ADPCM SNR unexpectedly low ({snr:.1f} dB)")

        vag_bytes = pack_vag_file(payload, file_stem, SAMPLE_RATE)
        vag_path = sfx_dir / f"{file_stem}.vag"
        vag_path.write_bytes(vag_bytes)

        if args.dump_wav is not None:
            write_wav_preview(args.dump_wav / f"{file_stem}.wav", pcm, SAMPLE_RATE)

        dur = len(pcm) / SAMPLE_RATE
        total_duration += dur
        total_spu_bytes += len(payload)
        entries.append((enum_name, file_stem, payload))

        print(
            f"  [{idx:2d}] {enum_name:<16s} -> {vag_path.name:<20s} | "
            f"{dur:.2f}s | {len(payload):6,d} B ({len(payload) // 16:4d} blks) | ADPCM SNR={snr:5.1f} dB"
        )

    if total_spu_bytes > MAX_SPU_BUDGET_BYTES:
        raise RuntimeError(
            f"Total SPU RAM payload {total_spu_bytes:,} B exceeds {MAX_SPU_BUDGET_BYTES:,} B budget!"
        )

    generate_c_files(out_dir, entries)

    print(
        f"[build_audio] SUCCESS: {len(entries)} VAG SFX + sfx_table.h + sfx_blobs.c\n"
        f"  Total Duration : {total_duration:.2f}s @ {SAMPLE_RATE} Hz mono\n"
        f"  Total SPU RAM  : {total_spu_bytes:,} bytes ({total_spu_bytes / 1024.0:.1f} KiB / 512 KiB PS1 SPU RAM, "
        f"{total_spu_bytes * 100.0 / (512 * 1024):.1f}% used)\n"
        f"  C API          : {out_dir / 'sfx_table.h'} & {out_dir / 'sfx_blobs.c'}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"[build_audio] ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
