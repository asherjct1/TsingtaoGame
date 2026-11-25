# TSINGTAO EXPRESS '96 (PlayStation 1 — PAL Real Hardware)

An original 1990s promotional-style PlayStation 1 arcade beer-serving game engineered to boot and run natively on **real PAL PlayStation 1 hardware** (`50 Hz`, `320×256`, Mode2/2352 multi-track `.bin`/`.cue` with Red Book CD-DA Track 02 audio and Slot 1 Memory Card saving).

Step behind the wooden taps of Qingdao's busiest street bar as the cool sunglasses-wearing Tsingtao mascot bartender. Serve ice-cold cans, green bottles, and iconic Qingdao plastic beer bags across 4 bustling counters as the sky shifts from Golden Sunset to Dusk, Electric Neon Rush, and Midnight Beer Festival Fever!

---

## Controls (Port 1 Digital / DualShock Controller)

| Button | In-Game Action | Menu / Bonus Action |
|---|---|---|
| **D-Pad Up / Down** | Instantly hop between the 4 bar counters | Navigate menu / cycle initials (`A–Z`, `0–9`) |
| **D-Pad Left / Right** | Dash along the current counter to grab Cash Tips & catch empties early | Select can in Bonus Round / move initial cursor |
| **Cross (`X`)** | Slide a **Tsingtao Can** down the counter | Confirm selection / start |
| **Circle (`O`)** | Slide a **Tsingtao Green Bottle** (for Bottle VIPs) | Cancel / back |
| **Square (`□`)** | Slide a **Qingdao Plastic Beer Bag** (for Bag VIPs) | — |
| **Triangle (`△`)** | Trigger **ICE-COLD RUSH** when Frost Gauge reaches `100%`! | — |
| **Start** | Pause menu (`RESUME`, `TOGGLE MUSIC`, `QUIT TO TITLE`) | Start shift from Title Screen |
| **Select** | — | Toggle Top 5 High Scores on Title Screen |

---

## Gameplay Guide

### Serving & Classic Arcade Stakes
- **Match the Drink:** Serve **Tsingtao Cans (`X`)** to Regular Locals, Big Foodies, and Rushers. Watch the overhead order bubbles on VIPs—serve **Green Bottles (`O`)** to Bottle VIPs and **Plastic Beer Bags (`□`)** to Bag VIPs!
- **Wrong Drink Penalty:** Serving the wrong drink to a VIP annoys them and speeds them up—stay sharp!
- **Catch Returning Empties:** Satisfied customers slide empty cans and bottles back toward your tap end. Catch them (by standing at the tap or dashing out along the bar) before they fall off the right edge!
- **No Wasted Pours:** Never slide a drink down an empty counter—if a drink smashes off the far left end without hitting a patron, you lose a life!
- **Defend the Tap:** If a thirsty patron reaches the tap end of the counter, they slam the bar and cost you a life (the counter clears briefly so you are never spawn-trapped).
- **Cash Tips & Combos:** Happy patrons drop **Cash Tips** on the bar. Dash left with the D-Pad to scoop them up for bonus points, combo extensions, and extra **Frost Gauge** charge.
- **ICE-COLD RUSH (`△`):** Fill the Frost Gauge to `100%` and press **Triangle (`△`)** to blast all 4 counters simultaneously with sparkling ice-cold Tsingtao cans—sweeping every patron and returning empty off-screen for huge bonus points!

### Customer Archetypes
1. **Regular Local** — Wants 1 **Tsingtao Can (`X`)**. Pushed back cleanly and slides back an empty can.
2. **Big Night-Market Foodie** — Wants **2 Tsingtao Cans (`X`)**. First can knocks them back; second can clears them for big points.
3. **Bottle VIP** *(Green Bottle bubble)* — Fast patron who insists on a **Tsingtao Green Bottle (`O`)**. High points and frequent cash tips.
4. **Qingdao Bag VIP** *(Beer Bag bubble)* — Loves an authentic **Qingdao Plastic Beer Bag (`□`)**. Grants big points and a huge Frost Gauge boost.
5. **Late-Night Rusher** *(Wave 4+)* — Sprints down the counter demanding a quick **Tsingtao Can (`X`)**!

### Dynamic Qingdao Atmosphere
- **Waves 1–2:** Warm Qingdao Golden Sunset over Zhanqiao Pier Pavilion
- **Waves 3–4:** Purple & Amber Dusk (red street lanterns ignite!)
- **Waves 5–6:** Electric Neon Late-Night Rush (pulsing neon wall sign & twinkling stars)
- **Wave 7+:** Midnight Qingdao Beer Festival Fever!

### Bonus Round: "SPOT THE COLD ONE!" (After Every 3 Waves)
Every 3 waves (Wave 3, 6, 9...), test your eyes in a classic 4-can shell game:
- 4 large digitized **Tsingtao Cans** sit on the bar.
- 3 cans are mischievously shaken while **1 ice-cold can gleams unshaken**.
- Track the cold one through escalating parabolic arc-swap shuffles, then pick with **Left/Right + Cross (`X`)**:
  - **Cold Unshaken Can:** `+5,000 PTS` + **100% FULL FROST RUSH** for the next wave!
  - **Shaken Can:** Pressurized foam fountain blasts the mascot's sunglasses (pure 90s comedy—no life lost!).

### Memory Card High Scores (Slot 1)
- Scoring in the Top 5 brings up classic arcade **3-letter initial entry**.
- Scores save automatically to **Slot 1 Memory Card** as `bu00:BESLES-09600TSING` (1 block, 8 KB) complete with a custom **16×16 Tsingtao Can icon** visible inside the official PlayStation 1 BIOS Memory Card Manager.
- If no Memory Card is inserted, your high scores remain saved for the current session without interrupting gameplay.

---

## Swapping Raw Tsingtao Photos & Music

All source graphics and music live in `assets/raw/` and are compiled deterministically with **zero AI image generation**:

- `assets/raw/tsingtao_logo.png` — Real Tsingtao Brewery logo (converted to `128×64` 8-bit PS1 `.TIM`)
- `assets/raw/tsingtao_can.png` — Real Tsingtao green can photo (converted to `32×48` bonus can & `16×24` gameplay can)
- `assets/raw/tsingtao_bottle.png` — Real Tsingtao green bottle photo (converted to `16×28` gameplay bottle)
- `assets/raw/bgm.wav` — Track 02 Red Book CD-DA background music (`44100 Hz`, `16-bit signed LE PCM`, stereo WAV)

To replace any image or swap the soundtrack:
1. Drop your replacement PNG into `assets/raw/` (keeping the exact filename above) or convert your custom music to `44.1 kHz` 16-bit stereo PCM WAV at `assets/raw/bgm.wav`:
   ```bash
   ffmpeg -i your_track.mp3 -ar 44100 -ac 2 -sample_fmt s16 -f wav assets/raw/bgm.wav
   ```
2. Rebuild the assets and PAL disc image:
   ```bash
   make assets
   make iso
   ```
The resulting multi-track disc image is written to `build/tsingtao_express_96.bin` and `build/tsingtao_express_96.cue`.

---

## Building from Source (macOS Apple Silicon)

```bash
# 1. One-time toolchain install (mipsel-none-elf-gcc, PSn00bSDK, mkpsxiso)
./scripts/setup_toolchain.sh

# 2. Generate zero-AI TIM graphics, SPU ADPCM VAG SFX, and Red Book BGM
make assets
make audio

# 3. Compile SLES_096.00 and package PAL Mode2/2352 + CD-DA disc image
make iso
```

---

## Running on Real PAL PlayStation 1 Hardware

`build/tsingtao_express_96.bin` + `build/tsingtao_express_96.cue` is a genuine **PAL (SCEE)** PlayStation 1 Mode2/2352 multi-track image (`SLES_096.00`) with Red Book CD-DA Track 02. All graphics (`.TIM`) and sound effects (`.VAG`) are loaded into VRAM and SPU RAM at boot so the CD laser streams Track 02 continuously with **zero mid-game disc seeks**.

> **Note on Retail Consoles:** Original PlayStation 1 optical drives check for a factory-pressed wobble groove (`SCEE` region signature). Booting a homemade CD-R on real hardware requires a **modchip**, a **FreePSXBoot / Unirom / Tonyhax** memory card exploit, or an optical drive emulator (**xStation** / **PSIO**).

### Option A: Burning to CD-R (Modchip or FreePSXBoot / Unirom / Tonyhax)
1. Use a quality 700 MB **CD-R** (not CD-RW — original PS1 lasers struggle with CD-RW reflectivity).
2. Always select the **`.cue`** file (`build/tsingtao_express_96.cue`), never the `.bin` alone, so Track 02 CD-DA audio is indexed accurately.
3. Burn in **DAO (Disc-At-Once)** mode at the **lowest supported burn speed** (typically `4x`–`8x`, or your drive's minimum supported speed).
   - **Windows (ImgBurn):** Choose *Write image file to disc* → select `tsingtao_express_96.cue` → set Write Speed to `4x` (or `8x`) → enable *Verify* → click Write.
   - **macOS / Linux (`cdrdao`):**
     ```bash
     cdrdao write --device /dev/cdrom --speed 4 --eject build/tsingtao_express_96.cue
     ```
4. Insert into your PAL PS1:
   - **Modchipped PAL PS1:** Power on directly—the game boots straight from `SLES_096.00` in PAL 50 Hz.
   - **Unmodded PAL PS1 + FreePSXBoot / Unirom / Tonyhax:** Insert your exploit Memory Card in Slot 2 (leaving Slot 1 free for Tsingtao Express '96 high score saves!), boot into Unirom/Tonyhax, insert the burned CD-R, and launch disc.

### Option B: Optical Drive Emulators (xStation / PSIO)
- **xStation:** Copy both `tsingtao_express_96.bin` and `tsingtao_express_96.cue` into a folder on your FAT32/exFAT SD card (e.g., `/Tsingtao Express 96/`), select the `.cue` entry in the xStation loader, and boot.
- **PSIO:** Place `tsingtao_express_96.bin` and `tsingtao_express_96.cue` in a folder on your PSIO SD card (PSIO natively supports multi-track `.bin`/`.cue` with CD-DA Track 02).

### Option C: Emulators (DuckStation / Mednafen / PCSX-Redux)
- Load `build/tsingtao_express_96.cue` directly (not `SLES_096.00` alone, or Track 02 CD-DA music won't be mounted).
- Use a PAL BIOS (or Auto region detection) so the emulator runs at native **50 Hz PAL** (`320×256`).
