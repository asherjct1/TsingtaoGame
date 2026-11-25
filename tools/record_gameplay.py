#!/usr/bin/env python3
"""Smart PS1-RAM-aware verification runner & gameplay recorder for TSINGTAO EXPRESS '96.

Boots build/tsingtao_express_96.cue on headless SwanStation Libretro, reads the
live PS1 2MB RAM `TelemetrySnapshot` (`g_telemetry`, magic 0x54534732 "TSG2")
via `retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)` using the address resolved
from `build/SLES_096.map`, plays reactively across every roguelike & arcade
state, enforces `--audit` invariants + display framing checks, captures key
PNG snapshots, and encodes a 50 FPS PAL MP4 with CD-DA + SPU audio.
"""

import argparse
import ctypes
import os
import re
import struct
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

ROOT = Path(__file__).resolve().parent.parent
CORE_PATH = ROOT / "tools" / "libretro" / "swanstation_libretro.dylib"
CUE_PATH = ROOT / "build" / "tsingtao_express_96.cue"
MAP_PATH = ROOT / "build" / "SLES_096.map"
SYS_DIR = ROOT / "tools" / "libretro" / "system"
SAVE_DIR = ROOT / "tools" / "libretro" / "saves"
FFMPEG_BIN = "/opt/homebrew/bin/ffmpeg"

# Libretro constants
RETRO_ENVIRONMENT_GET_OVERSCAN = 2
RETRO_ENVIRONMENT_GET_CAN_DUPE = 3
RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY = 9
RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10
RETRO_ENVIRONMENT_GET_VARIABLE = 15
RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE = 17
RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY = 31

RETRO_PIXEL_FORMAT_0RGB1555 = 0
RETRO_PIXEL_FORMAT_XRGB8888 = 1
RETRO_PIXEL_FORMAT_RGB565 = 2

RETRO_MEMORY_SYSTEM_RAM = 2
PS1_RAM_SIZE = 2 * 1024 * 1024

RETRO_DEVICE_JOYPAD = 1
# Libretro joypad button IDs -> PS1 DualShock/Digital mapping
BTN_B = 0       # PS1 Cross (X) - Tsingtao Can / Confirm
BTN_Y = 1       # PS1 Square ([]) - Qingdao Beer Bag / Sell
BTN_SELECT = 2  # PS1 Select
BTN_START = 3   # PS1 Start
BTN_UP = 4      # D-Pad Up
BTN_DOWN = 5    # D-Pad Down
BTN_LEFT = 6    # D-Pad Left
BTN_RIGHT = 7   # D-Pad Right
BTN_A = 8       # PS1 Circle (O) - Tsingtao Green Bottle / Back
BTN_X = 9       # PS1 Triangle (^) - Ice-Cold Rush
BTN_L = 10      # PS1 L1 - Pocket Trick 0
BTN_R = 11      # PS1 R1 - Pocket Trick 1

TELEMETRY_MAGIC = 0x54534732  # "TSG2"

# AppState enum (verified against src/main.c)
STATE_TITLE = 0
STATE_STARTER_DRAFT = 1
STATE_PLAYING = 2
STATE_PAUSED = 3
STATE_PAYOUT = 4
STATE_BONUS = 5
STATE_VICTORY = 6
STATE_SHOP = 7
STATE_GAME_OVER = 8
STATE_NAME_ENTRY = 9
STATE_HIGH_SCORES = 10

STATE_NAMES = {
    STATE_TITLE: "TITLE",
    STATE_STARTER_DRAFT: "STARTER_DRAFT",
    STATE_PLAYING: "PLAYING",
    STATE_PAUSED: "PAUSED",
    STATE_PAYOUT: "PAYOUT",
    STATE_BONUS: "BONUS",
    STATE_VICTORY: "VICTORY",
    STATE_SHOP: "SHOP",
    STATE_GAME_OVER: "GAME_OVER",
    STATE_NAME_ENTRY: "NAME_ENTRY",
    STATE_HIGH_SCORES: "HIGH_SCORES",
}

# BonusPhase enum (verified against src/bonus.h)
BONUS_PHASE_INTRO = 0
BONUS_PHASE_SHAKE = 1
BONUS_PHASE_SHUFFLE = 2
BONUS_PHASE_PICK = 3
BONUS_PHASE_REVEAL = 4
BONUS_PHASE_REWARD = 5
BONUS_PHASE_DONE = 6

# TrickId enum (from src/roguelike.h)
TRICK_NONE = -1
TRICK_COLD_MUG = 0
TRICK_HAPPY_BELL = 1
TRICK_BOUNCER = 2
TRICK_ICE_BUCKET = 3
TRICK_FOAM_CATCH = 4
TRICK_VIP_COUPON = 5

DRINK_TO_BTN = {
    0: BTN_B,  # Can -> Cross
    1: BTN_A,  # Bottle -> Circle
    2: BTN_Y,  # Bag -> Square
}


class TelemetrySnapshot(ctypes.LittleEndianStructure):
    """Exact packed mirror of C `volatile TelemetrySnapshot g_telemetry`."""
    _pack_ = 1
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("frame", ctypes.c_uint32),
        ("app_state", ctypes.c_uint8),
        ("night", ctypes.c_uint8),
        ("shift_in_night", ctypes.c_uint8),
        ("boss_id", ctypes.c_uint8),
        ("lives", ctypes.c_int8),
        ("bartender_lane", ctypes.c_int8),
        ("bartender_x", ctypes.c_uint8),
        ("frost", ctypes.c_uint8),
        ("cash", ctypes.c_int16),
        ("shift_frames_left", ctypes.c_int16),
        ("shift_score", ctypes.c_uint32),
        ("shift_target", ctypes.c_uint32),
        ("run_score", ctypes.c_uint32),
        ("combo", ctypes.c_uint16),
        ("coaster_count", ctypes.c_uint8),
        ("tricks", ctypes.c_int8 * 2),
        ("shift_tips_spawned", ctypes.c_uint8),
        ("shift_tips_cash", ctypes.c_int16),
        ("shift_spills", ctypes.c_uint8),
        ("bonus_phase", ctypes.c_uint8),
        ("bonus_cursor", ctypes.c_int8),
        ("bonus_cold_slot", ctypes.c_int8),
        ("bonus_reward_cursor", ctypes.c_int8),
        ("draft_cursor", ctypes.c_uint8),
        ("shop_zone", ctypes.c_uint8),
        ("shop_cursor", ctypes.c_uint8),
        ("shop_reroll_cost", ctypes.c_int16),
        ("shop_sold_mask", ctypes.c_uint8),
        ("lane_cust_count", ctypes.c_uint8 * 4),
        ("lane_front_x", ctypes.c_uint8 * 4),
        ("lane_front_drink", ctypes.c_uint8 * 4),
        ("lane_front_hp", ctypes.c_uint8 * 4),
        ("lane_empty_count", ctypes.c_uint8 * 4),
        ("lane_empty_x", ctypes.c_uint8 * 4),
        ("lane_tip_active", ctypes.c_uint8 * 4),
        ("lane_tip_x", ctypes.c_uint8 * 4),
        ("lane_drink_count", ctypes.c_uint8 * 4),
    ]


class RetroGameInfo(ctypes.Structure):
    _fields_ = [
        ("path", ctypes.c_char_p),
        ("data", ctypes.c_void_p),
        ("size", ctypes.c_size_t),
        ("meta", ctypes.c_char_p),
    ]


class RetroVariable(ctypes.Structure):
    _fields_ = [
        ("key", ctypes.c_char_p),
        ("value", ctypes.c_char_p),
    ]


class RetroSystemTiming(ctypes.Structure):
    _fields_ = [
        ("fps", ctypes.c_double),
        ("sample_rate", ctypes.c_double),
    ]


class RetroGameGeometry(ctypes.Structure):
    _fields_ = [
        ("base_width", ctypes.c_uint),
        ("base_height", ctypes.c_uint),
        ("max_width", ctypes.c_uint),
        ("max_height", ctypes.c_uint),
        ("aspect_ratio", ctypes.c_float),
    ]


class RetroSystemAvInfo(ctypes.Structure):
    _fields_ = [
        ("geometry", RetroGameGeometry),
        ("timing", RetroSystemTiming),
    ]


ENV_CB = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)
VIDEO_CB = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_size_t)
AUDIO_SAMPLE_CB = ctypes.CFUNCTYPE(None, ctypes.c_int16, ctypes.c_int16)
AUDIO_BATCH_CB = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t)
INPUT_POLL_CB = ctypes.CFUNCTYPE(None)
INPUT_STATE_CB = ctypes.CFUNCTYPE(ctypes.c_int16, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint)


def parse_telemetry_ram_offset(map_path: Path, symbol: str = "g_telemetry") -> Tuple[int, Optional[int]]:
    """Parse symbol hex address from build/SLES_096.map and strip 0x80000000 mask."""
    if not map_path.exists():
        raise FileNotFoundError(f"Map file not found: {map_path}")
    text = map_path.read_text(encoding="utf-8", errors="replace")
    pattern = re.compile(rf"^\s*{re.escape(symbol)}\s+\S+\s+(?:0x)?([0-9a-fA-F]+)(?:\s+([0-9a-fA-F]+))?", re.MULTILINE)
    m = pattern.search(text)
    if not m:
        raise RuntimeError(f"Symbol '{symbol}' not found in {map_path}")
    vaddr = int(m.group(1), 16)
    sym_size = int(m.group(2), 16) if m.group(2) else None
    ram_offset = vaddr & 0x001FFFFF
    if ram_offset + ctypes.sizeof(TelemetrySnapshot) > PS1_RAM_SIZE:
        raise RuntimeError(f"Telemetry RAM offset 0x{ram_offset:06x} exceeds 2MB PS1 RAM")
    return ram_offset, sym_size


def read_telemetry(core: ctypes.CDLL, ram_offset: int) -> Optional[TelemetrySnapshot]:
    """Read TelemetrySnapshot directly from PS1 2MB System RAM."""
    ptr = core.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)
    if not ptr:
        return None
    size = core.retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM)
    if size < ram_offset + ctypes.sizeof(TelemetrySnapshot):
        return None
    raw = ctypes.string_at(ptr + ram_offset, ctypes.sizeof(TelemetrySnapshot))
    snap = TelemetrySnapshot.from_buffer_copy(raw)
    return snap


def analyze_frame_framing(frame_bytes: bytes, width: int, height: int, pixel_format: int) -> Tuple[bool, str]:
    """Verify bottom 14px control strip is unclipped/visible and no oversized top black bar exists."""
    bpp = 4 if pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 else 2
    row_bytes = width * bpp
    if len(frame_bytes) < row_bytes * height:
        return False, f"short framebuffer {len(frame_bytes)} < {row_bytes * height}"

    nonblack_rows: List[int] = []
    bright_rows: List[int] = []
    very_bright_rows: List[int] = []

    x_start = 8
    x_end = max(x_start + 1, width - 8)
    span = x_end - x_start

    for y in range(height):
        row = frame_bytes[y * row_bytes : y * row_bytes + row_bytes]
        nb = 0
        br = 0
        vb = 0
        if pixel_format == RETRO_PIXEL_FORMAT_RGB565:
            vals = struct.unpack_from(f"<{span}H", row, x_start * 2)
            for v in vals:
                r = ((v >> 11) & 31) * 8
                g = ((v >> 5) & 63) * 4
                b = (v & 31) * 8
                if r + g + b > 20:
                    nb += 1
                m = r if r >= g and r >= b else (g if g >= b else b)
                if m >= 120:
                    br += 1
                if m >= 170:
                    vb += 1
        elif pixel_format == RETRO_PIXEL_FORMAT_0RGB1555:
            vals = struct.unpack_from(f"<{span}H", row, x_start * 2)
            for v in vals:
                r = ((v >> 10) & 31) * 8
                g = ((v >> 5) & 31) * 8
                b = (v & 31) * 8
                if r + g + b > 20:
                    nb += 1
                m = r if r >= g and r >= b else (g if g >= b else b)
                if m >= 120:
                    br += 1
                if m >= 170:
                    vb += 1
        else:
            for x in range(x_start * 4, x_end * 4, 4):
                b, g, r = row[x], row[x + 1], row[x + 2]
                if r + g + b > 20:
                    nb += 1
                m = r if r >= g and r >= b else (g if g >= b else b)
                if m >= 120:
                    br += 1
                if m >= 170:
                    vb += 1
        nonblack_rows.append(nb)
        bright_rows.append(br)
        very_bright_rows.append(vb)

    leading_black = 0
    while leading_black < height and nonblack_rows[leading_black] <= 4:
        leading_black += 1

    last_nonblack = height - 1
    while last_nonblack >= 0 and nonblack_rows[last_nonblack] <= 4:
        last_nonblack -= 1

    if leading_black > 12:
        return False, f"excessive top black bar: {leading_black} blank rows (PAL screen.y shift?)"

    if last_nonblack < 200:
        return False, f"frame content ends early at row {last_nonblack}/{height}"

    # Top HUD region: rows leading_black + 2 .. leading_black + 38
    top_lo = min(height - 1, leading_black + 2)
    top_hi = min(height, leading_black + 40)
    top_bright_sum = sum(bright_rows[top_lo:top_hi])
    if top_bright_sum < 40:
        return False, f"top HUD missing bright elements in rows {top_lo}..{top_hi} (sum={top_bright_sum})"

    # Bottom control strip region: expected around game y=233..247 -> leading_black + 233..247
    bot_lo = max(0, min(height - 24, leading_black + 228))
    bot_hi = min(height, leading_black + 252)
    strip_bright_rows = [y for y in range(bot_lo, bot_hi) if bright_rows[y] >= 12 or very_bright_rows[y] >= 8]
    if len(strip_bright_rows) < 4:
        return False, (
            f"bottom control strip missing or obscured in rows {bot_lo}..{bot_hi} "
            f"(bright rows={len(strip_bright_rows)}, leading_black={leading_black})"
        )

    # Verify the bottom strip isn't sliced off at the very last output scanline
    if very_bright_rows[height - 1] >= 12 and bright_rows[height - 2] >= 12 and leading_black >= 14:
        return False, f"bottom control strip clipped against bottom border (row {height - 1} has bright glyphs)"

    if leading_black + 249 > height:
        return False, f"bottom 248px safe zone extends past height {height} (leading_black={leading_black})"

    return True, f"ok (top_blank={leading_black}, bot_strip={strip_bright_rows[0]}..{strip_bright_rows[-1]}/{height})"


class PS1ArcadeBot:
    """Reactive PS1-RAM-aware arcade player across all states."""

    def __init__(self) -> None:
        self.prev_state: Optional[int] = None
        self.state_enter_gf: int = 0
        self.prev_bonus_phase: Optional[int] = None
        self.bonus_phase_enter_gf: int = 0
        self.last_pulse_gf: Dict[int, int] = {}
        self.last_pour_gf: int = -100
        self.last_trick_gf: int = -100
        self.draft_inspected: bool = False
        self.shop_tried_slots: Set[int] = set()
        self.shop_wait_until_gf: int = 0
        self.shop_rerolled_this_visit: bool = False
        self.payout_presses: int = 0

    def _can_pulse(self, btn: int, gf: int, cooldown: int = 5) -> bool:
        return (gf - self.last_pulse_gf.get(btn, -100)) >= cooldown

    def _pulse(self, pressed: Set[int], btn: int, gf: int, cooldown: int = 5) -> bool:
        if self._can_pulse(btn, gf, cooldown):
            pressed.add(btn)
            self.last_pulse_gf[btn] = gf
            return True
        return False

    def step(self, gf: int, t: Optional[TelemetrySnapshot]) -> Set[int]:
        pressed: Set[int] = set()
        if t is None or t.magic != TELEMETRY_MAGIC:
            return pressed

        st = int(t.app_state)
        if st != self.prev_state:
            self.prev_state = st
            self.state_enter_gf = gf
            if st == STATE_STARTER_DRAFT:
                self.draft_inspected = False
            elif st == STATE_SHOP:
                self.shop_tried_slots = set()
                self.shop_wait_until_gf = gf + 40
                self.shop_rerolled_this_visit = False
            elif st == STATE_PAYOUT:
                self.payout_presses = 0
            elif st == STATE_BONUS:
                self.prev_bonus_phase = None

        dwell = gf - self.state_enter_gf

        if st == STATE_TITLE:
            # Show animated title screen for ~1.8 seconds before pressing Start
            if dwell >= 90:
                self._pulse(pressed, BTN_START, gf, cooldown=20)
            return pressed

        if st == STATE_STARTER_DRAFT:
            # Wait for 3D cards to flip open, inspect second card, then confirm
            if dwell < 42:
                return pressed
            if not self.draft_inspected and t.draft_cursor == 0:
                if self._pulse(pressed, BTN_RIGHT, gf, cooldown=14):
                    self.draft_inspected = True
                return pressed
            if dwell >= 62:
                self._pulse(pressed, BTN_B, gf, cooldown=14)
            return pressed

        if st == STATE_PAUSED:
            if dwell >= 10:
                self._pulse(pressed, BTN_START, gf, cooldown=15)
            return pressed

        if st == STATE_PAYOUT:
            # First press at dwell==35 reveals full ledger; second press at dwell>=65 advances
            if self.payout_presses == 0 and dwell >= 35:
                if self._pulse(pressed, BTN_B, gf, cooldown=14):
                    self.payout_presses += 1
            elif self.payout_presses >= 1 and dwell >= 65:
                if self._pulse(pressed, BTN_B, gf, cooldown=14):
                    self.payout_presses += 1
            return pressed

        if st == STATE_VICTORY:
            # Night 8 Boss victory screen: pick Endless Mode (cursor 0) after showcase pause
            if dwell >= 60:
                self._pulse(pressed, BTN_B, gf, cooldown=18)
            return pressed

        if st == STATE_BONUS:
            return self._step_bonus(gf, t)

        if st == STATE_SHOP:
            return self._step_shop(gf, dwell, t)

        if st in (STATE_GAME_OVER, STATE_NAME_ENTRY, STATE_HIGH_SCORES):
            if dwell >= 40:
                self._pulse(pressed, BTN_START if st != STATE_HIGH_SCORES else BTN_B, gf, cooldown=18)
            return pressed

        if st == STATE_PLAYING:
            return self._step_playing(gf, t)

        return pressed

    def _step_bonus(self, gf: int, t: TelemetrySnapshot) -> Set[int]:
        pressed: Set[int] = set()
        phase = int(t.bonus_phase)
        if phase != self.prev_bonus_phase:
            self.prev_bonus_phase = phase
            self.bonus_phase_enter_gf = gf
        pdwell = gf - self.bonus_phase_enter_gf

        if phase == BONUS_PHASE_INTRO:
            if pdwell >= 30:
                self._pulse(pressed, BTN_B, gf, cooldown=20)
        elif phase == BONUS_PHASE_PICK:
            if pdwell < 12:
                return pressed
            target = int(t.bonus_cold_slot)
            cur = int(t.bonus_cursor)
            if 0 <= target <= 3 and 0 <= cur <= 3:
                if cur < target:
                    self._pulse(pressed, BTN_RIGHT, gf, cooldown=6)
                elif cur > target:
                    self._pulse(pressed, BTN_LEFT, gf, cooldown=6)
                else:
                    self._pulse(pressed, BTN_B, gf, cooldown=14)
        elif phase == BONUS_PHASE_REVEAL:
            if pdwell >= 45:
                self._pulse(pressed, BTN_B, gf, cooldown=18)
        elif phase == BONUS_PHASE_REWARD:
            # Wait for 3D reward cards to finish flipping in (24 frames) before picking
            if pdwell < 30:
                return pressed
            desired = 0 if t.coaster_count < 5 else (1 if t.lives < 5 else 2)
            cur = int(t.bonus_reward_cursor)
            if 0 <= cur <= 2:
                if cur < desired:
                    self._pulse(pressed, BTN_RIGHT, gf, cooldown=8)
                elif cur > desired:
                    self._pulse(pressed, BTN_LEFT, gf, cooldown=8)
                else:
                    self._pulse(pressed, BTN_B, gf, cooldown=16)
        return pressed

    def _step_shop(self, gf: int, dwell: int, t: TelemetrySnapshot) -> Set[int]:
        """Shop navigation aligned with rl_shop_update (Zone 0=Tray, 1=Offers, 2=Actions)."""
        pressed: Set[int] = set()
        if gf < self.shop_wait_until_gf:
            return pressed

        zone = int(t.shop_zone)
        cursor = int(t.shop_cursor)
        sold = int(t.shop_sold_mask)
        cash = int(t.cash)

        # Pick next untried, unsold offer we could reasonably want (Drink=2, Trick=3, Coasters=0,1)
        desired_offer: Optional[int] = None
        if cash >= 3 and dwell < 320:
            for slot in (2, 3, 0, 1):
                if slot in self.shop_tried_slots:
                    continue
                if sold & (1 << slot):
                    self.shop_tried_slots.add(slot)
                    continue
                if slot in (0, 1) and t.coaster_count >= 5:
                    self.shop_tried_slots.add(slot)
                    continue
                if slot == 3 and (t.tricks[0] != TRICK_NONE and t.tricks[1] != TRICK_NONE):
                    self.shop_tried_slots.add(slot)
                    continue
                desired_offer = slot
                break

        if desired_offer is not None:
            if zone == 0:
                self._pulse(pressed, BTN_DOWN, gf, cooldown=5)
            elif zone == 2:
                self._pulse(pressed, BTN_UP, gf, cooldown=5)
            else:
                if cursor < desired_offer:
                    self._pulse(pressed, BTN_RIGHT, gf, cooldown=4)
                elif cursor > desired_offer:
                    self._pulse(pressed, BTN_LEFT, gf, cooldown=4)
                else:
                    if self._pulse(pressed, BTN_B, gf, cooldown=8):
                        self.shop_tried_slots.add(desired_offer)
                        self.shop_wait_until_gf = gf + 6
            return pressed

        # Reroll once if rich and all offers checked
        if (not self.shop_rerolled_this_visit) and cash >= int(t.shop_reroll_cost) + 5 and dwell < 160:
            if zone < 2:
                self._pulse(pressed, BTN_DOWN, gf, cooldown=5)
            else:
                if cursor != 0:
                    self._pulse(pressed, BTN_LEFT, gf, cooldown=4)
                else:
                    if self._pulse(pressed, BTN_B, gf, cooldown=10):
                        self.shop_rerolled_this_visit = True
                        self.shop_tried_slots = set()
                        self.shop_wait_until_gf = gf + 16
            return pressed

        # Navigate to NEXT SHIFT (zone 2, cursor 1) and advance
        if zone < 2:
            self._pulse(pressed, BTN_DOWN, gf, cooldown=5)
        elif cursor != 1:
            self._pulse(pressed, BTN_RIGHT, gf, cooldown=4)
        else:
            self._pulse(pressed, BTN_B, gf, cooldown=12)
        return pressed

    def _step_playing(self, gf: int, t: TelemetrySnapshot) -> Set[int]:
        pressed: Set[int] = set()
        total_cust = sum(int(t.lane_cust_count[i]) for i in range(4))
        max_front_x = max(int(t.lane_front_x[i]) if t.lane_cust_count[i] > 0 else 0 for i in range(4))
        max_empty_x = max(int(t.lane_empty_x[i]) if t.lane_empty_count[i] > 0 else 0 for i in range(4))

        # 1. Ice-Cold Rush on Triangle when Frost == 100 and 2+ customers are out
        if t.frost >= 100 and total_cust >= 2:
            if self._pulse(pressed, BTN_X, gf, cooldown=20):
                return pressed

        # 2. Pocket Tricks (L1 = slot 0, R1 = slot 1) used smartly
        if (gf - self.last_trick_gf) >= 45:
            for slot_idx, btn in ((0, BTN_L), (1, BTN_R)):
                tid = int(t.tricks[slot_idx])
                if tid == TRICK_NONE:
                    continue
                use_it = False
                if tid == TRICK_COLD_MUG and t.lives <= 3:
                    use_it = True
                elif tid == TRICK_BOUNCER and max_front_x >= 185:
                    use_it = True
                elif tid == TRICK_ICE_BUCKET and t.frost <= 45 and total_cust >= 2:
                    use_it = True
                elif tid == TRICK_HAPPY_BELL and total_cust >= 2 and t.shift_frames_left > 150:
                    use_it = True
                elif tid == TRICK_FOAM_CATCH and sum(int(t.lane_empty_count[i]) for i in range(4)) >= 1:
                    use_it = True
                elif tid == TRICK_VIP_COUPON and t.shift_frames_left > 250:
                    use_it = True
                if use_it and self._pulse(pressed, btn, gf, cooldown=20):
                    self.last_trick_gf = gf
                    return pressed

        # 3. Compute urgency per lane
        best_lane = int(t.bartender_lane)
        best_action = "IDLE"  # "EMPTY", "POUR", "TIP", "IDLE"
        best_score = -1

        lanes_safe_for_near_tips = (max_front_x < 160) and (max_empty_x < 170)
        lanes_safe_for_deep_tips = (max_front_x < 125) and (max_empty_x < 145)

        for l in range(4):
            cc = int(t.lane_cust_count[l])
            fx = int(t.lane_front_x[l])
            fhp = int(t.lane_front_hp[l])
            dc = int(t.lane_drink_count[l])
            ec = int(t.lane_empty_count[l])
            ex = int(t.lane_empty_x[l])
            ta = int(t.lane_tip_active[l])
            tx = int(t.lane_tip_x[l])

            # Imminent returning empty near tap
            if ec > 0 and ex >= 155:
                sc = 2500 + ex
                if sc > best_score:
                    best_score, best_lane, best_action = sc, l, "EMPTY"

            # Customer needs drink (never pour if dc >= 2 or dc >= fhp)
            if cc > 0 and dc < 2 and dc < fhp:
                # Extra urgency if customer is close to tap breach
                sc = (3200 + fx) if fx >= 190 else (1200 + fx)
                if sc > best_score:
                    best_score, best_lane, best_action = sc, l, "POUR"

            # Non-imminent empty waiting to be caught
            if ec > 0 and ex < 155:
                sc = 600 + ex
                if sc > best_score:
                    best_score, best_lane, best_action = sc, l, "EMPTY"

            # Gold Tip or beneficial Buff pickup when lanes are safe
            if ta > 0:
                if tx >= 110 and lanes_safe_for_near_tips and max_front_x < 150:
                    sc = 1500 + tx
                elif lanes_safe_for_deep_tips:
                    sc = 1400 + tx
                elif lanes_safe_for_near_tips:
                    sc = 300 + (255 - tx)
                else:
                    sc = -1
                if sc > best_score:
                    best_score, best_lane, best_action = sc, l, "TIP"

        cur_lane = int(t.bartender_lane)
        bx = int(t.bartender_x)

        if cur_lane != best_lane:
            if best_lane < cur_lane:
                self._pulse(pressed, BTN_UP, gf, cooldown=4)
            else:
                self._pulse(pressed, BTN_DOWN, gf, cooldown=4)
            return pressed

        if best_action == "EMPTY":
            ex = int(t.lane_empty_x[cur_lane])
            # Move toward tap/empty cleanly (holding Left/Right allowed continuously)
            if bx < ex - 6 or bx < 232:
                pressed.add(BTN_RIGHT)
            elif bx > ex + 14 and max_front_x < 180:
                pressed.add(BTN_LEFT)
            return pressed

        if best_action == "TIP":
            tx = int(t.lane_tip_x[cur_lane])
            if bx > tx + 4:
                pressed.add(BTN_LEFT)
            elif bx < tx - 4:
                pressed.add(BTN_RIGHT)
            return pressed

        if best_action == "POUR":
            drink_type = int(t.lane_front_drink[cur_lane])
            btn = DRINK_TO_BTN.get(drink_type, BTN_B)
            # Respect 8-frame tap-pull cadence & input buffer (auto-sprints right if bx < 248)
            if (gf - self.last_pour_gf) >= 8:
                if self._pulse(pressed, btn, gf, cooldown=5):
                    self.last_pour_gf = gf
            return pressed

        # Idle: return smoothly toward tap if left on counter
        if bx < 240:
            pressed.add(BTN_RIGHT)
        return pressed


@dataclass
class AuditReport:
    frames_checked: int = 0
    magic_seen: bool = False
    magic_first_gf: Optional[int] = None
    seen_states: Set[int] = field(default_factory=set)
    seen_shifts: Set[Tuple[int, int]] = field(default_factory=set)
    seen_bonus: bool = False
    seen_shop: bool = False
    seen_payout: bool = False
    max_night: int = 0
    max_run_score: int = 0
    max_cash: int = 0
    failures: List[str] = field(default_factory=list)
    framing_checks: int = 0
    framing_failures: List[str] = field(default_factory=list)

    def fail(self, msg: str) -> None:
        if len(self.failures) < 30 and msg not in self.failures:
            self.failures.append(msg)

    def fail_framing(self, msg: str) -> None:
        if len(self.framing_failures) < 15 and msg not in self.framing_failures:
            self.framing_failures.append(msg)


def save_snapshot_png(frame_bytes: bytes, w: int, h: int, pixel_format: int, out_png: Path) -> None:
    """Save crisp 3x integer nearest-neighbor scaled PNG using ffmpeg."""
    pix_fmt = (
        "rgb565le" if pixel_format == RETRO_PIXEL_FORMAT_RGB565
        else ("bgr0" if pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 else "rgb555le")
    )
    snap_raw = out_png.with_suffix(".raw")
    snap_raw.write_bytes(frame_bytes)
    try:
        subprocess.run([
            FFMPEG_BIN, "-y", "-loglevel", "error",
            "-f", "rawvideo", "-pix_fmt", pix_fmt, "-s", f"{w}x{h}",
            "-i", str(snap_raw),
            "-vf", f"scale={w * 3}:{h * 3}:flags=neighbor",
            str(out_png),
        ], check=True)
    finally:
        snap_raw.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="PS1-RAM-aware verifier & gameplay recorder for TSINGTAO EXPRESS '96.")
    parser.add_argument("--seconds", type=int, default=240, help="Gameplay duration in seconds (default: 240)")
    parser.add_argument("--output", type=str, default=str(ROOT / "build" / "tsingtao_express_96_v2_showcase.mp4"), help="Output MP4 path")
    parser.add_argument("--snap-dir", type=str, default="", help="Directory to save key PNG snapshots")
    parser.add_argument("--audit", action="store_true", help="Assert telemetry, economy, progression, and display framing invariants")
    parser.add_argument("--map", type=str, default=str(MAP_PATH), help="Path to SLES_096.map")
    parser.add_argument("--cue", type=str, default=str(CUE_PATH), help="Path to CUE image")
    args = parser.parse_args()

    cue_path = Path(args.cue)
    map_path = Path(args.map)
    if not CORE_PATH.exists():
        print(f"ERROR: Core not found at {CORE_PATH}", file=sys.stderr)
        sys.exit(1)
    if not cue_path.exists():
        print(f"ERROR: CUE not found at {cue_path}", file=sys.stderr)
        sys.exit(1)

    ram_offset, sym_size = parse_telemetry_ram_offset(map_path, "g_telemetry")
    expected_size = ctypes.sizeof(TelemetrySnapshot)
    print(f"Parsed g_telemetry RAM offset: 0x{ram_offset:06x} (struct={expected_size}B, map_size={sym_size})")
    if sym_size is not None and sym_size != expected_size:
        msg = f"TelemetrySnapshot size mismatch: Python={expected_size}B vs map={sym_size}B"
        if args.audit:
            print(f"ERROR: {msg}", file=sys.stderr)
            sys.exit(1)
        print(f"WARNING: {msg}", file=sys.stderr)

    SYS_DIR.mkdir(parents=True, exist_ok=True)
    SAVE_DIR.mkdir(parents=True, exist_ok=True)
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    raw_vid_path = out_path.with_suffix(".video_only.mp4")
    raw_aud_path = out_path.with_suffix(".audio.pcm")

    snap_dir = Path(args.snap_dir) if args.snap_dir else None
    if snap_dir:
        snap_dir.mkdir(parents=True, exist_ok=True)

    sys_dir_b = str(SYS_DIR).encode("utf-8")
    save_dir_b = str(SAVE_DIR).encode("utf-8")

    state = {
        "pixel_format": RETRO_PIXEL_FORMAT_RGB565,
        "frame": 0,
        "game_frame": 0,
        "recording": False,
        "buttons": set(),
        "next_buttons": set(),
        "vid_w": 320,
        "vid_h": 256,
        "ffmpeg": None,
        "audio_file": open(raw_aud_path, "wb"),
        "audio_frames": 0,
        "last_frame_bytes": None,
        "pending_snap_name": None,
    }

    captured_snaps: Dict[str, Path] = {}

    core_vars = {
        b"swanstation_GPU_Renderer": b"Software",
        b"swanstation_GPU_ResolutionScale": b"1",
        b"swanstation_Console_Region": b"PAL",
        b"swanstation_CPU_ExecutionMode": b"CachedInterpreter",
        b"swanstation_BIOS_PatchFastBoot": b"false",
    }

    @ENV_CB
    def env_cb(cmd, data):
        if cmd == RETRO_ENVIRONMENT_GET_OVERSCAN:
            ctypes.cast(data, ctypes.POINTER(ctypes.c_bool))[0] = True
            return True
        if cmd == RETRO_ENVIRONMENT_GET_CAN_DUPE:
            ctypes.cast(data, ctypes.POINTER(ctypes.c_bool))[0] = True
            return True
        if cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            fmt = ctypes.cast(data, ctypes.POINTER(ctypes.c_uint))[0]
            state["pixel_format"] = fmt
            return True
        if cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            ctypes.cast(data, ctypes.POINTER(ctypes.c_char_p))[0] = sys_dir_b
            return True
        if cmd == RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            ctypes.cast(data, ctypes.POINTER(ctypes.c_char_p))[0] = save_dir_b
            return True
        if cmd == RETRO_ENVIRONMENT_GET_VARIABLE:
            var = ctypes.cast(data, ctypes.POINTER(RetroVariable)).contents
            if var.key in core_vars:
                var.value = core_vars[var.key]
                return True
            return False
        if cmd == RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            ctypes.cast(data, ctypes.POINTER(ctypes.c_bool))[0] = False
            return True
        return False

    def start_ffmpeg(w: int, h: int, fmt: int) -> None:
        pix_fmt = (
            "rgb565le" if fmt == RETRO_PIXEL_FORMAT_RGB565
            else ("bgr0" if fmt == RETRO_PIXEL_FORMAT_XRGB8888 else "rgb555le")
        )
        cmd = [
            FFMPEG_BIN,
            "-y",
            "-loglevel", "error",
            "-f", "rawvideo",
            "-pix_fmt", pix_fmt,
            "-s", f"{w}x{h}",
            "-r", "50",
            "-i", "pipe:0",
            "-vf", f"scale={w * 3}:{h * 3}:flags=neighbor",
            "-c:v", "libx264",
            "-preset", "veryfast",
            "-crf", "18",
            "-pix_fmt", "yuv420p",
            str(raw_vid_path),
        ]
        state["vid_w"] = w
        state["vid_h"] = h
        state["ffmpeg"] = subprocess.Popen(cmd, stdin=subprocess.PIPE)

    @VIDEO_CB
    def video_cb(data, width, height, pitch):
        if not state["recording"]:
            if data and width == 320 and height >= 240:
                state["recording"] = True
                start_ffmpeg(width, height, state["pixel_format"])
            else:
                return
        if not data:
            if state["ffmpeg"] and state["last_frame_bytes"]:
                state["ffmpeg"].stdin.write(state["last_frame_bytes"])
            return
        bpp = 4 if state["pixel_format"] == RETRO_PIXEL_FORMAT_XRGB8888 else 2
        row_bytes = width * bpp

        target_w, target_h = state["vid_w"], state["vid_h"]
        target_row = target_w * bpp

        raw = ctypes.string_at(data, pitch * height)
        if pitch == target_row and height == target_h:
            frame_bytes = raw
        else:
            out = bytearray(target_row * target_h)
            copy_h = min(height, target_h)
            copy_w = min(row_bytes, target_row)
            for y in range(copy_h):
                src_off = y * pitch
                dst_off = y * target_row
                out[dst_off:dst_off + copy_w] = raw[src_off:src_off + copy_w]
            frame_bytes = bytes(out)

        state["last_frame_bytes"] = frame_bytes
        state["ffmpeg"].stdin.write(frame_bytes)

    @AUDIO_SAMPLE_CB
    def audio_sample_cb(left, right):
        if not state["recording"]:
            return
        state["audio_file"].write(int(left).to_bytes(2, "little", signed=True))
        state["audio_file"].write(int(right).to_bytes(2, "little", signed=True))
        state["audio_frames"] += 1

    @AUDIO_BATCH_CB
    def audio_batch_cb(data, frames):
        if state["recording"] and data and frames > 0:
            state["audio_file"].write(ctypes.string_at(data, frames * 4))
            state["audio_frames"] += frames
        return frames

    @INPUT_POLL_CB
    def input_poll_cb():
        state["buttons"] = set(state["next_buttons"]) if state["recording"] else set()

    @INPUT_STATE_CB
    def input_state_cb(port, device, index, btn_id):
        if port == 0 and device == RETRO_DEVICE_JOYPAD:
            return 1 if btn_id in state["buttons"] else 0
        return 0

    core = ctypes.CDLL(str(CORE_PATH))
    core.retro_get_memory_data.argtypes = [ctypes.c_uint]
    core.retro_get_memory_data.restype = ctypes.c_void_p
    core.retro_get_memory_size.argtypes = [ctypes.c_uint]
    core.retro_get_memory_size.restype = ctypes.c_size_t

    core.retro_set_environment(env_cb)
    core.retro_set_video_refresh(video_cb)
    core.retro_set_audio_sample(audio_sample_cb)
    core.retro_set_audio_sample_batch(audio_batch_cb)
    core.retro_set_input_poll(input_poll_cb)
    core.retro_set_input_state(input_state_cb)

    core.retro_init()
    game = RetroGameInfo(path=str(cue_path).encode("utf-8"), data=None, size=0, meta=None)
    core.retro_load_game.restype = ctypes.c_bool
    if not core.retro_load_game(ctypes.byref(game)):
        print("ERROR: retro_load_game failed!", file=sys.stderr)
        sys.exit(1)

    av_info = RetroSystemAvInfo()
    core.retro_get_system_av_info(ctypes.byref(av_info))
    sample_rate = int(round(av_info.timing.sample_rate)) or 44100
    fps = av_info.timing.fps or 50.0
    total_frames = int(args.seconds * 50)
    print(f"Loaded {cue_path.name} | Core FPS={fps:.2f}, Audio={sample_rate} Hz | Recording {total_frames} frames ({args.seconds}s)...")

    bot = PS1ArcadeBot()
    audit = AuditReport()
    last_state_change_gf = 0
    prev_app_state: Optional[int] = None

    t0 = time.time()
    f = 0
    while state["game_frame"] < total_frames:
        state["frame"] = f
        core.retro_run()
        f += 1

        if not state["recording"]:
            if f > 2000:
                print("ERROR: Game never switched to 320x256!", file=sys.stderr)
                os._exit(1)
            continue

        gf = state["game_frame"]
        state["game_frame"] += 1

        t = read_telemetry(core, ram_offset)
        valid_magic = (t is not None and t.magic == TELEMETRY_MAGIC)

        if valid_magic:
            if not audit.magic_seen:
                audit.magic_seen = True
                audit.magic_first_gf = gf
                print(f"  [Telemetry] TSG2 magic active at game frame {gf}", flush=True)

            st = int(t.app_state)
            audit.frames_checked += 1
            audit.seen_states.add(st)
            audit.max_night = max(audit.max_night, int(t.night))
            audit.max_run_score = max(audit.max_run_score, int(t.run_score))
            audit.max_cash = max(audit.max_cash, int(t.cash))

            if st == STATE_PLAYING:
                audit.seen_shifts.add((int(t.night), int(t.shift_in_night)))
            elif st == STATE_BONUS:
                audit.seen_bonus = True
            elif st == STATE_SHOP:
                audit.seen_shop = True
            elif st == STATE_PAYOUT:
                audit.seen_payout = True

            if st != prev_app_state:
                print(
                    f"  [State] gf={gf:5d} -> {STATE_NAMES.get(st, str(st))} "
                    f"(N{t.night} S{t.shift_in_night + 1} boss={t.boss_id} lives={t.lives} cash=${t.cash} score={t.run_score})",
                    flush=True,
                )
                prev_app_state = st
                last_state_change_gf = gf

            # Audit invariants
            if args.audit:
                if t.cash < 0:
                    audit.fail(f"gf={gf}: negative cash={t.cash}")
                if t.shift_tips_spawned > 6:
                    audit.fail(f"gf={gf}: shift_tips_spawned={t.shift_tips_spawned} > 6")
                if t.shift_tips_cash > 10:
                    audit.fail(f"gf={gf}: shift_tips_cash={t.shift_tips_cash} > 10")
                if t.run_score > 999999999:
                    audit.fail(f"gf={gf}: run_score={t.run_score} > 999999999")
                if t.lives > 5 or t.lives < 0:
                    audit.fail(f"gf={gf}: invalid lives={t.lives}")
                if t.coaster_count > 5:
                    audit.fail(f"gf={gf}: coaster_count={t.coaster_count} > 5")
                if st != STATE_PLAYING and (gf - last_state_change_gf) > 1500:
                    audit.fail(f"gf={gf}: softlock in state {STATE_NAMES.get(st, st)} for {gf - last_state_change_gf} frames")

            # Capture key snapshots
            dwell = gf - last_state_change_gf
            total_cust = sum(int(t.lane_cust_count[i]) for i in range(4))
            has_empty = any(int(t.lane_empty_count[i]) > 0 for i in range(4))
            has_tip_or_buff = any(int(t.lane_tip_active[i]) > 0 for i in range(4))
            max_fx = max(int(t.lane_front_x[i]) if t.lane_cust_count[i] > 0 else 0 for i in range(4))
            max_ex = max(int(t.lane_empty_x[i]) if t.lane_empty_count[i] > 0 else 0 for i in range(4))
            snap_name: Optional[str] = None
            if snap_dir and state["last_frame_bytes"] is not None:
                if st == STATE_TITLE and "title.png" not in captured_snaps and dwell >= 45:
                    snap_name = "title.png"
                elif st == STATE_STARTER_DRAFT and "draft.png" not in captured_snaps and dwell >= 40:
                    snap_name = "draft.png"
                elif (
                    st == STATE_PLAYING and t.night == 1 and t.shift_in_night == 0
                    and "gameplay_sunset.png" not in captured_snaps
                    and ((dwell >= 260 and total_cust >= 2) or dwell >= 450)
                ):
                    snap_name = "gameplay_sunset.png"
                elif (
                    st == STATE_PLAYING and dwell >= 150
                    and "gameplay_danger_empty.png" not in captured_snaps
                    and has_empty and (max_fx >= 160 or max_ex >= 145)
                ):
                    snap_name = "gameplay_danger_empty.png"
                elif (
                    st == STATE_PLAYING and dwell >= 150
                    and "gameplay_buff_and_tip.png" not in captured_snaps
                    and has_tip_or_buff and t.combo >= 2
                ):
                    snap_name = "gameplay_buff_and_tip.png"
                elif (
                    st == STATE_PLAYING and dwell >= 180
                    and "gameplay_combo_buff.png" not in captured_snaps
                    and has_tip_or_buff and t.combo >= 5
                ):
                    snap_name = "gameplay_combo_buff.png"
                elif (
                    st == STATE_PLAYING and (t.night >= 2 or t.shift_in_night == 1)
                    and t.shift_in_night != 2
                    and "gameplay_dusk_or_neon.png" not in captured_snaps
                    and ((dwell >= 260 and total_cust >= 2) or dwell >= 450)
                ):
                    snap_name = "gameplay_dusk_or_neon.png"
                elif (
                    st == STATE_PLAYING and t.shift_in_night == 2
                    and "boss_shift.png" not in captured_snaps
                    and ((dwell >= 260 and total_cust >= 2) or dwell >= 450)
                ):
                    snap_name = "boss_shift.png"
                elif (
                    st == STATE_PLAYING and t.night >= 2
                    and "night2_start.png" not in captured_snaps
                    and dwell >= 90
                ):
                    snap_name = "night2_start.png"
                elif st == STATE_PAYOUT and "payout.png" not in captured_snaps and dwell >= 45:
                    snap_name = "payout.png"
                elif (
                    st == STATE_BONUS and t.bonus_phase == BONUS_PHASE_PICK
                    and "bonus_shell.png" not in captured_snaps
                    and bot.prev_bonus_phase == BONUS_PHASE_PICK
                    and (gf - bot.bonus_phase_enter_gf) >= 4
                ):
                    snap_name = "bonus_shell.png"
                elif (
                    st == STATE_BONUS and t.bonus_phase == BONUS_PHASE_REWARD
                    and "bonus_reward.png" not in captured_snaps
                    and bot.prev_bonus_phase == BONUS_PHASE_REWARD
                    and (gf - bot.bonus_phase_enter_gf) >= 26
                ):
                    snap_name = "bonus_reward.png"
                elif st == STATE_SHOP and "shop.png" not in captured_snaps and dwell >= 35:
                    snap_name = "shop.png"

            if snap_name and snap_dir:
                out_png = snap_dir / snap_name
                save_snapshot_png(state["last_frame_bytes"], state["vid_w"], state["vid_h"], state["pixel_format"], out_png)
                captured_snaps[snap_name] = out_png
                print(f"  [Snap] Saved {out_png}", flush=True)
                if args.audit and st in (STATE_TITLE, STATE_STARTER_DRAFT, STATE_PLAYING, STATE_SHOP):
                    ok_f, reason_f = analyze_frame_framing(state["last_frame_bytes"], state["vid_w"], state["vid_h"], state["pixel_format"])
                    audit.framing_checks += 1
                    if not ok_f:
                        audit.fail_framing(f"{snap_name} (gf={gf}): {reason_f}")

            # Periodic framing audit every 50 frames in core interactive screens (wait >=2 frames after state change for PS1 double-buffer flip)
            if (
                args.audit
                and state["last_frame_bytes"] is not None
                and dwell >= 2
                and (gf % 50 == 25)
                and st in (STATE_TITLE, STATE_STARTER_DRAFT, STATE_PLAYING, STATE_SHOP)
            ):
                ok_f, reason_f = analyze_frame_framing(state["last_frame_bytes"], state["vid_w"], state["vid_h"], state["pixel_format"])
                audit.framing_checks += 1
                if not ok_f:
                    audit.fail_framing(f"gf={gf} ({STATE_NAMES.get(st, st)}): {reason_f}")

        else:
            if args.audit:
                if audit.magic_seen:
                    audit.fail(f"gf={gf}: TSG2 magic lost (got 0x{(t.magic if t else 0):08x})")
                elif gf > 200:
                    audit.fail(f"gf={gf}: TSG2 magic never activated after boot")

        state["next_buttons"] = bot.step(gf, t)

        if gf > 0 and (gf % 2500 == 0 or gf + 1 == total_frames):
            elapsed = time.time() - t0
            speed = (gf / 50.0) / max(elapsed, 0.01)
            print(f"  Game Frame {gf + 1}/{total_frames} ({(gf + 1) / 50.0:.1f}s game time) in {elapsed:.1f}s ({speed:.1f}x realtime)", flush=True)

    if state["ffmpeg"]:
        state["ffmpeg"].stdin.close()
        state["ffmpeg"].wait()
    state["audio_file"].close()

    print(f"Muxing video + {state['audio_frames']} audio frames ({sample_rate} Hz) -> {out_path}...", flush=True)
    subprocess.run([
        FFMPEG_BIN, "-y", "-loglevel", "error",
        "-i", str(raw_vid_path),
        "-f", "s16le", "-ar", str(sample_rate), "-ac", "2", "-i", str(raw_aud_path),
        "-c:v", "copy",
        "-c:a", "aac", "-b:a", "192k",
        "-shortest",
        "-movflags", "+faststart",
        str(out_path),
    ], check=True)

    raw_vid_path.unlink(missing_ok=True)
    raw_aud_path.unlink(missing_ok=True)
    print(f"DONE! Saved {out_path} ({out_path.stat().st_size / (1024 * 1024):.1f} MB)", flush=True)

    if args.audit:
        if not audit.magic_seen:
            audit.fail("TSG2 telemetry magic never became active")
        if STATE_STARTER_DRAFT not in audit.seen_states:
            audit.fail("Never reached STATE_STARTER_DRAFT")
        if (1, 0) not in audit.seen_shifts or (1, 1) not in audit.seen_shifts or (1, 2) not in audit.seen_shifts:
            audit.fail(f"Did not visit all Night 1 Shifts (0,1,2); saw {sorted(audit.seen_shifts)}")
        if not audit.seen_payout:
            audit.fail("Never reached STATE_PAYOUT")
        if not audit.seen_shop:
            audit.fail("Never reached STATE_SHOP")
        if not audit.seen_bonus:
            audit.fail("Never reached STATE_BONUS")
        if audit.max_night < 2:
            audit.fail(f"Did not progress into Night 2+ (max_night={audit.max_night})")

        print("\n=== VERIFICATION AUDIT SUMMARY ===")
        print(f"  Telemetry frames : {audit.frames_checked} (TSG2 active at gf={audit.magic_first_gf})")
        print(f"  States visited   : {[STATE_NAMES.get(s, s) for s in sorted(audit.seen_states)]}")
        print(f"  Shifts played    : {sorted(audit.seen_shifts)}")
        print(f"  Max Night / Score: Night {audit.max_night} | Score {audit.max_run_score} | Peak Cash ${audit.max_cash}")
        print(f"  Framing checks   : {audit.framing_checks} checked, {len(audit.framing_failures)} failed")
        print(f"  Snapshots saved  : {sorted(captured_snaps.keys())}")
        if audit.failures or audit.framing_failures:
            for msg in audit.failures + audit.framing_failures:
                print(f"  [FAIL] {msg}", file=sys.stderr)
            os._exit(1)
        print("  AUDIT RESULT: PASS (0 failures)")

    os._exit(0)


if __name__ == "__main__":
    main()
