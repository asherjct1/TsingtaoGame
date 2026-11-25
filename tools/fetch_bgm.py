#!/usr/bin/env python3
"""
fetch_bgm.py — Download and transcode Track 02 Red Book CD-DA BGM for TSINGTAO EXPRESS '96.

Source: https://www.youtube.com/watch?v=NUI9nqWX_EI (Balatro Theme - 80's sound edit)
Output: assets/raw/bgm.wav (strict Red Book CD-DA: 44100 Hz, 16-bit signed LE PCM, 2ch stereo,
        normalized to -1.0 dB peak, clean fade-in/out, minimal standard WAV header).
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEFAULT_URL = "https://www.youtube.com/watch?v=NUI9nqWX_EI"
PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUTPUT = PROJECT_ROOT / "assets" / "raw" / "bgm.wav"
VENV_YTDLP = PROJECT_ROOT / "tools" / ".audio_venv" / "bin" / "yt-dlp"


def find_binary(preferred: str, fallback_name: str) -> str:
    if preferred and os.path.isfile(preferred) and os.access(preferred, os.X_OK):
        return preferred
    found = shutil.which(fallback_name)
    if found:
        return found
    raise FileNotFoundError(f"Required tool '{fallback_name}' not found (checked {preferred} and PATH).")


def run_cmd(cmd, capture=True):
    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True,
    )
    return proc


def probe_audio(ffprobe_bin: str, wav_path: Path) -> dict:
    cmd = [
        ffprobe_bin,
        "-v", "error",
        "-select_streams", "a:0",
        "-show_entries", "stream=codec_name,sample_rate,channels,bits_per_sample,duration:format=duration,size",
        "-of", "json",
        str(wav_path),
    ]
    proc = run_cmd(cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"ffprobe failed on {wav_path}: {proc.stderr.strip()}")
    data = json.loads(proc.stdout)
    streams = data.get("streams", [])
    if not streams:
        raise RuntimeError(f"No audio stream found in {wav_path}")
    stream = streams[0]
    fmt = data.get("format", {})
    duration = float(stream.get("duration") or fmt.get("duration") or 0.0)
    return {
        "codec_name": stream.get("codec_name", ""),
        "sample_rate": int(stream.get("sample_rate", 0)),
        "channels": int(stream.get("channels", 0)),
        "bits_per_sample": int(stream.get("bits_per_sample", 0)),
        "duration": duration,
        "size": int(fmt.get("size") or wav_path.stat().st_size),
    }


def verify_redbook_wav(ffprobe_bin: str, wav_path: Path) -> dict:
    info = probe_audio(ffprobe_bin, wav_path)
    errors = []
    if info["codec_name"] != "pcm_s16le":
        errors.append(f"codec_name={info['codec_name']} (expected pcm_s16le)")
    if info["sample_rate"] != 44100:
        errors.append(f"sample_rate={info['sample_rate']} (expected 44100)")
    if info["channels"] != 2:
        errors.append(f"channels={info['channels']} (expected 2)")
    if info["bits_per_sample"] != 16:
        errors.append(f"bits_per_sample={info['bits_per_sample']} (expected 16)")
    if info["duration"] <= 10.0:
        errors.append(f"duration={info['duration']:.2f}s (expected > 10.0s)")
    if errors:
        raise RuntimeError("Red Book CD-DA verification failed: " + "; ".join(errors))
    return info


def detect_max_volume_db(ffmpeg_bin: str, input_path: Path) -> float:
    cmd = [
        ffmpeg_bin,
        "-hide_banner",
        "-i", str(input_path),
        "-af", "volumedetect",
        "-vn",
        "-f", "null",
        "-",
    ]
    proc = run_cmd(cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg volumedetect failed: {proc.stderr.strip()}")
    match = re.search(r"max_volume:\s*(-?\d+(?:\.\d+)?)\s*dB", proc.stderr)
    if not match:
        return 0.0
    return float(match.group(1))


def transcode_to_redbook(ffmpeg_bin: str, ffprobe_bin: str, src_path: Path, dst_path: Path) -> None:
    src_info = probe_audio(ffprobe_bin, src_path)
    duration = src_info["duration"]
    if duration <= 10.0:
        raise RuntimeError(f"Source audio too short ({duration:.2f}s <= 10s): {src_path}")

    max_vol_db = detect_max_volume_db(ffmpeg_bin, src_path)
    # Normalize peak to -1.0 dB (avoid extreme boost if silence)
    target_peak_db = -1.0
    gain_db = max(-24.0, min(24.0, target_peak_db - max_vol_db))

    fade_in = 0.08
    fade_out = 1.20
    fade_out_start = max(0.0, duration - fade_out)

    # Trim to exact CD-DA sector multiple (588 samples = 1/75 sec at 44100 Hz)
    total_sectors = int(duration * 75.0)
    aligned_duration = total_sectors / 75.0

    af_chain = (
        f"volume={gain_db:.2f}dB,"
        f"afade=t=in:st=0:d={fade_in:.2f},"
        f"afade=t=out:st={fade_out_start:.3f}:d={fade_out:.2f}"
    )

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_wav = dst_path.with_suffix(".tmp.wav")

    cmd = [
        ffmpeg_bin,
        "-y",
        "-hide_banner",
        "-loglevel", "error",
        "-i", str(src_path),
        "-vn",
        "-map_metadata", "-1",
        "-fflags", "+bitexact",
        "-af", af_chain,
        "-t", f"{aligned_duration:.6f}",
        "-ar", "44100",
        "-ac", "2",
        "-sample_fmt", "s16",
        "-acodec", "pcm_s16le",
        "-f", "wav",
        str(tmp_wav),
    ]
    print(f"[fetch_bgm] Transcoding to Red Book CD-DA (peak {max_vol_db:.2f} dB -> {target_peak_db:.1f} dB, gain {gain_db:+.2f} dB)...")
    proc = run_cmd(cmd)
    if proc.returncode != 0:
        if tmp_wav.exists():
            tmp_wav.unlink()
        raise RuntimeError(f"ffmpeg transcode failed: {proc.stderr.strip()}")

    tmp_wav.replace(dst_path)


def download_youtube_audio(ytdlp_bin: str, ffmpeg_bin: str, url: str, tmp_dir: Path) -> Path:
    out_template = str(tmp_dir / "source.%(ext)s")
    ffmpeg_dir = str(Path(ffmpeg_bin).parent)

    # Prefer android,web player client first to avoid YouTube web SABR issues
    attempts = [
        [
            ytdlp_bin,
            "--no-playlist",
            "--ffmpeg-location", ffmpeg_dir,
            "--extractor-args", "youtube:player_client=android,web",
            "-f", "bestaudio/best",
            "-o", out_template,
            url,
        ],
        [
            ytdlp_bin,
            "--no-playlist",
            "--ffmpeg-location", ffmpeg_dir,
            "-f", "bestaudio/best",
            "-o", out_template,
            url,
        ],
    ]

    last_err = ""
    for idx, cmd in enumerate(attempts, 1):
        print(f"[fetch_bgm] Downloading audio via yt-dlp (attempt {idx}/{len(attempts)}): {url}")
        proc = run_cmd(cmd)
        if proc.returncode == 0:
            files = [p for p in tmp_dir.iterdir() if p.is_file() and p.name.startswith("source.")]
            if files:
                return files[0]
            last_err = "yt-dlp exited 0 but produced no output file."
        else:
            last_err = (proc.stderr or proc.stdout).strip()
            print(f"[fetch_bgm] Attempt {idx} failed:\n{last_err}", file=sys.stderr)

    raise RuntimeError(f"YouTube download failed via yt-dlp.\nLast error:\n{last_err}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Fetch & transcode Red Book CD-DA BGM for TSINGTAO EXPRESS '96.")
    parser.add_argument("--url", default=DEFAULT_URL, help="YouTube URL to download.")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Output WAV path.")
    parser.add_argument("--force", action="store_true", help="Re-download and overwrite existing bgm.wav.")
    args = parser.parse_args()

    ffmpeg_bin = find_binary("/opt/homebrew/bin/ffmpeg", "ffmpeg")
    ffprobe_bin = find_binary("/opt/homebrew/bin/ffprobe", "ffprobe")

    out_path: Path = args.output.resolve()

    if out_path.exists() and not args.force:
        print(f"[fetch_bgm] Existing file found at {out_path}; skipping download and verifying format...")
        info = verify_redbook_wav(ffprobe_bin, out_path)
        print(
            f"[fetch_bgm] OK (existing): {out_path} | "
            f"{info['codec_name']} {info['sample_rate']} Hz {info['channels']}ch {info['bits_per_sample']}-bit | "
            f"duration={info['duration']:.2f}s | size={info['size']:,} bytes"
        )
        return 0

    # Check if user dropped a local non-WAV bgm.* file in assets/raw/
    raw_dir = out_path.parent
    local_override = None
    if not args.force and raw_dir.exists():
        for ext in (".mp3", ".m4a", ".flac", ".ogg", ".opus", ".aac"):
            cand = raw_dir / f"bgm{ext}"
            if cand.exists():
                local_override = cand
                break

    with tempfile.TemporaryDirectory(prefix="tsingtao_bgm_") as td:
        tmp_dir = Path(td)
        if local_override is not None:
            print(f"[fetch_bgm] Using local drop-in audio: {local_override}")
            src_audio = local_override
        else:
            ytdlp_bin = find_binary(str(VENV_YTDLP), "yt-dlp")
            src_audio = download_youtube_audio(ytdlp_bin, ffmpeg_bin, args.url, tmp_dir)

        transcode_to_redbook(ffmpeg_bin, ffprobe_bin, src_audio, out_path)

    info = verify_redbook_wav(ffprobe_bin, out_path)
    peak_after = detect_max_volume_db(ffmpeg_bin, out_path)
    print(
        f"[fetch_bgm] SUCCESS: {out_path}\n"
        f"  Format   : {info['codec_name']} (Red Book CD-DA)\n"
        f"  Rate/Ch  : {info['sample_rate']} Hz, {info['channels']}ch stereo, {info['bits_per_sample']}-bit\n"
        f"  Duration : {info['duration']:.2f}s ({int(round(info['duration'] * 75))} CD sectors)\n"
        f"  Peak     : {peak_after:.2f} dB\n"
        f"  Size     : {info['size']:,} bytes ({info['size'] / (1024 * 1024):.2f} MiB)"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"[fetch_bgm] ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
