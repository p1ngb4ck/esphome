#!/usr/bin/env python3
"""Transcode an arbitrary input video into the MJPEG-in-AVI format simple_video_player expects.

Container: AVI. Video: MJPEG (baseline JPEG per frame, ffmpeg default 4:2:0 chroma -- the
ESP32-P4 hardware JPEG decoder accepts 4:2:0/4:2:2/4:4:4/grayscale via its own header parsing,
so no special pixel format forcing is required). Audio: PCM (raw samples, no WAV header -- AVI's
own chunk framing carries it) / MP3 / FLAC, matching exactly what AVIAudioCodec
(esphome/components/simple_video_player/avi_parser.h) recognizes. No other audio codec is
understood by the player, so none is offered here.

Requires ffmpeg on PATH.
"""

import argparse
import shutil
import subprocess
import sys

AUDIO_CODECS = ("pcm", "mp3", "flac", "none")


def build_ffmpeg_args(args: argparse.Namespace) -> list[str]:
    ffmpeg_args: list[str] = ["ffmpeg", "-y", "-i", args.input]

    # Video: MJPEG, explicit pixel format for determinism, fps and (optional) resolution.
    # -colorspace bt601 pins the YUV<->RGB matrix explicitly: MJPEG has no reliable in-band
    # colorspace signaling, and without this ffmpeg's own default can vary by resolution (BT.601
    # for SD-ish content, BT.709 for HD-ish -- and 1280x800 sits right at that boundary). The
    # ESP32-P4 hardware decoder's jpeg_decode_cfg_t.conv_std is explicitly set to
    # JPEG_YUV_RGB_CONV_STD_BT601 to match (see decode_frame_backend_<HW_P4> in
    # simple_video_player.cpp) -- a mismatch here would show up as inaccurate (not necessarily
    # broken-looking) colors, particularly reds/blues.
    ffmpeg_args += ["-c:v", "mjpeg", "-pix_fmt", "yuvj420p", "-colorspace", "bt601", "-r", str(args.fps)]
    if args.qscale is not None:
        ffmpeg_args += ["-q:v", str(args.qscale)]
    if args.width and args.height:
        ffmpeg_args += ["-vf", f"scale={args.width}:{args.height}"]
    elif args.width or args.height:
        # ffmpeg scale filter accepts -1 for "keep aspect ratio" on the other axis.
        ffmpeg_args += ["-vf", f"scale={args.width or -1}:{args.height or -1}"]

    # Audio
    if args.audio_codec == "none":
        ffmpeg_args += ["-an"]
    else:
        ffmpeg_args += ["-ar", str(args.sample_rate), "-ac", str(args.channels)]
        if args.audio_codec == "pcm":
            ffmpeg_args += ["-c:a", "pcm_s16le"]
        elif args.audio_codec == "mp3":
            ffmpeg_args += ["-c:a", "libmp3lame", "-b:a", args.audio_bitrate]
        elif args.audio_codec == "flac":
            ffmpeg_args += ["-c:a", "flac"]

    ffmpeg_args += ["-f", "avi", args.output]
    return ffmpeg_args


def estimate_buffers(args: argparse.Namespace) -> None:
    """Rough sizing hints for simple_video_player's input_buffer_size / prefetch_frames."""
    if not (args.width and args.height):
        return
    # Very rough JPEG size estimate at "reasonable" quality: ~0.15-0.3 bytes/pixel for 4:2:0
    # baseline JPEG at typical photographic content. Printed as a starting point, not a
    # guarantee -- actual frame size depends heavily on content and the chosen -q:v.
    pixels = args.width * args.height
    low = int(pixels * 0.15)
    high = int(pixels * 0.30)
    print(
        f"Estimated per-frame JPEG size at {args.width}x{args.height}: "
        f"~{low // 1024}-{high // 1024} KB (content-dependent, not a guarantee).\n"
        f"simple_video_player's input_buffer_size should comfortably clear the high end -- "
        f"e.g. round up to the next 64KB step above {high // 1024}KB.",
        file=sys.stderr,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("input", help="Input video file (any format ffmpeg can read)")
    parser.add_argument("output", help="Output .avi file for simple_video_player")
    parser.add_argument(
        "--width", type=int, default=None, help="Output width (default: keep source)"
    )
    parser.add_argument(
        "--height", type=int, default=None, help="Output height (default: keep source)"
    )
    parser.add_argument("--fps", type=float, default=25.0, help="Output frame rate (default: 25)")
    parser.add_argument(
        "--qscale",
        type=int,
        default=7,
        help="MJPEG quality, ffmpeg -q:v scale 2 (best) - 31 (worst). Default: 7 -- the HW JPEG "
        "decoder's per-frame time is resolution-bound (fixed hardware pipeline), not file-size-"
        "bound, so lower quality here buys smaller storage reads and buffer margin with little "
        "real downside on a small embedded panel. Raise towards 5 if artifacts are visible.",
    )
    parser.add_argument(
        "--audio-codec",
        choices=AUDIO_CODECS,
        default="pcm",
        help="Audio codec embedded in the AVI stream. Must be one AVIAudioCodec recognizes "
        "(pcm/mp3/flac) or 'none' for video-only. Default: pcm",
    )
    parser.add_argument(
        "--sample-rate", type=int, default=48000, help="Audio sample rate (default: 48000)"
    )
    parser.add_argument(
        "--channels", type=int, default=2, help="Audio channel count (default: 2, stereo)"
    )
    parser.add_argument(
        "--audio-bitrate",
        default="192k",
        help="Audio bitrate, only used for --audio-codec mp3 (default: 192k)",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="Print the ffmpeg command without running it"
    )
    args = parser.parse_args()

    if not args.dry_run and shutil.which("ffmpeg") is None:
        print("ffmpeg not found on PATH.", file=sys.stderr)
        return 1

    ffmpeg_args = build_ffmpeg_args(args)
    print("Running:", " ".join(ffmpeg_args), file=sys.stderr)
    estimate_buffers(args)

    if args.dry_run:
        return 0

    result = subprocess.run(ffmpeg_args)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
