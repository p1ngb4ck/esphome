#!/usr/bin/env python3
"""Realtime-transcode a live video stream into MJPEG-in-AVI for simple_video_player.

Three source kinds are supported:
  - rtsp:    Any RTSP source, audio+video. This is also what go2rtc (Home Assistant's modern
             default streaming backend, bundled since 2024.x) exposes for every camera, by
             default on port 8554: rtsp://<ha-host>:8554/<camera-stream-name>
  - generic: Any other URI GStreamer's uridecodebin can negotiate -- HLS (.m3u8), RTMP,
             progressive HTTP (MP4/MKV/etc.), local files, audio+video. The general-purpose
             fallback for "other sources" beyond RTSP/HA specifically.
  - http-mjpeg: HA's universal, backend-independent, video-ONLY endpoint, always available
             regardless of which streaming backend a camera uses:
             http://<ha-host>:8123/api/camera_proxy_stream/<entity_id>?token=<token>
             (token from Developer Tools, or a long-lived access token as a query/header)

rtsp and generic both demux via a named dynamic-pad element (decodebin / uridecodebin) and can
carry both an audio and a video stream; http-mjpeg is fixed to video-only (there is no audio in
that protocol) and always forces --audio-codec to none.

Video is always re-encoded to MJPEG (jpegenc) -- resolution/fps/quality are pipeline
parameters. Audio: pcm (raw, verified against avimux) or mp3 (lamemp3enc, verified) or none.
FLAC through avimux is NOT verified here (unlike the ffmpeg-based file transcoder, where FLAC-in-
AVI was confirmed against ffmpeg's own codec tag table) -- test a short capture before relying on
it if you pass --audio-codec flac.

This produces a *finished* AVI file once stopped (Ctrl+C or --duration) -- it is a capture tool,
not a live-append-while-reading stream; simple_video_player has no live-ingestion path today, it
only reads complete files via storage.

Requires gst-launch-1.0 (GStreamer) on PATH, with the rtsp/soup-http-src/uridecodebin, jpeg, avi
and (for mp3) lame plugins installed.
"""

import argparse
from pathlib import Path
import shutil
import signal
import subprocess
import sys

AUDIO_CODECS = ("pcm", "mp3", "flac", "none")


def build_pipeline(args: argparse.Namespace) -> list[str]:
    # rtsp and generic sources may carry both audio and video, requiring two separate downstream
    # chains off named dynamic pads -- decodebin/uridecodebin's "name=demux" + bare "demux."
    # references are the standard idiom for that. http-mjpeg (HA's camera_proxy_stream) is
    # always video-only (audio_codec is forced to "none" for it above), so it only ever needs
    # one linear chain -- no named-pad linking required.
    if args.source_type == "rtsp":
        src = [
            "rtspsrc",
            f"location={args.source}",
            "latency=200",
            "!",
            "decodebin",
            "name=demux",
        ]
        video_chain = ["demux.", "!"]
    elif args.source_type == "generic":
        src = ["uridecodebin", f"uri={_to_uri(args.source)}", "name=demux"]
        video_chain = ["demux.", "!"]
    else:  # http-mjpeg
        src = [
            "souphttpsrc",
            f"location={args.source}",
            "!",
            "multipartdemux",
            "!",
            "jpegdec",
        ]
        video_chain = ["!"]

    video_chain += [
        "videoconvert",
        "!",
        "videorate",
        "!",
        "videoscale",
        "!",
    ]
    caps = f"video/x-raw,framerate={_fraction(args.fps)}"
    if args.width and args.height:
        caps += f",width={args.width},height={args.height}"
    video_chain += [caps, "!", "jpegenc", f"quality={args.jpeg_quality}", "!", "mux."]

    pipeline = ["gst-launch-1.0", "-e"]
    pipeline += src
    pipeline += video_chain

    if args.audio_codec != "none":
        audio_chain = ["demux.", "!", "audioconvert", "!", "audioresample", "!"]
        audio_caps = f"audio/x-raw,rate={args.sample_rate},channels={args.channels}"
        audio_chain += [audio_caps, "!"]
        if args.audio_codec == "pcm":
            pass  # raw audio/x-raw straight into avimux -- verified accepted
        elif args.audio_codec == "mp3":
            audio_chain += ["lamemp3enc", f"bitrate={args.audio_bitrate_kbps}", "!"]
        elif args.audio_codec == "flac":
            audio_chain += ["flacenc", "!"]  # NOT verified against avimux, test first
        audio_chain += ["mux."]
        pipeline += audio_chain

    pipeline += ["avimux", "name=mux", "!", "filesink", f"location={args.output}"]
    return pipeline


def _to_uri(source: str) -> str:
    """uridecodebin needs a real URI scheme -- accept a bare local path for convenience."""
    if "://" in source:
        return source
    return Path(source).resolve().as_uri()


def _fraction(fps: float) -> str:
    # GStreamer caps want a fraction, not a float.
    if fps == int(fps):
        return f"{int(fps)}/1"
    # Simple 1000-denominator approximation, good enough for typical fps values (23.976 etc.
    # would need proper rational reduction, but that's not a case this tool targets).
    return f"{int(fps * 1000)}/1000"


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "source", help="RTSP/HLS/RTMP/HTTP URL, HA camera_proxy_stream URL, or local file path"
    )
    parser.add_argument("output", help="Output .avi file")
    parser.add_argument(
        "--source-type",
        choices=("rtsp", "generic", "http-mjpeg"),
        default=None,
        help="Defaults to auto-detect: rtsp:// -> rtsp; a URL containing 'camera_proxy_stream' "
        "-> http-mjpeg; anything else -> generic (uridecodebin, handles HLS/RTMP/progressive "
        "HTTP/local files, with audio if present)",
    )
    parser.add_argument("--width", type=int, default=None)
    parser.add_argument("--height", type=int, default=None)
    parser.add_argument("--fps", type=float, default=25.0)
    parser.add_argument(
        "--jpeg-quality", type=int, default=85, help="jpegenc quality, 0-100 (default: 85)"
    )
    parser.add_argument("--audio-codec", choices=AUDIO_CODECS, default="pcm")
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--channels", type=int, default=2)
    parser.add_argument("--audio-bitrate-kbps", type=int, default=192)
    parser.add_argument(
        "--duration", type=float, default=None, help="Stop after N seconds (default: run until Ctrl+C)"
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.source_type is None:
        if args.source.startswith("rtsp://"):
            args.source_type = "rtsp"
        elif "camera_proxy_stream" in args.source:
            args.source_type = "http-mjpeg"
        else:
            args.source_type = "generic"

    if args.source_type == "http-mjpeg" and args.audio_codec != "none":
        print(
            "HA's camera_proxy_stream (http-mjpeg) is video-only -- there is no audio track to "
            "extract. Forcing --audio-codec none for this source type.",
            file=sys.stderr,
        )
        args.audio_codec = "none"

    if not args.dry_run and shutil.which("gst-launch-1.0") is None:
        print("gst-launch-1.0 not found on PATH.", file=sys.stderr)
        return 1

    pipeline = build_pipeline(args)
    print("Running:", " ".join(pipeline), file=sys.stderr)
    if args.audio_codec == "flac":
        print(
            "WARNING: avimux + flacenc has not been verified to work -- test a short capture "
            "before relying on it.",
            file=sys.stderr,
        )

    if args.dry_run:
        return 0

    if not args.duration:
        return subprocess.run(pipeline).returncode

    # gst-launch-1.0 -e finalizes the AVI's index/header on SIGINT/SIGBREAK -- a hard kill
    # (subprocess.run(timeout=...)'s default behavior) would leave a corrupted file, so this
    # sends a graceful interrupt after --duration and waits for it to exit on its own.
    popen_kwargs = {}
    if sys.platform == "win32":
        popen_kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    proc = subprocess.Popen(pipeline, **popen_kwargs)
    try:
        proc.wait(timeout=args.duration)
        return proc.returncode
    except subprocess.TimeoutExpired:
        pass

    stop_signal = signal.CTRL_BREAK_EVENT if sys.platform == "win32" else signal.SIGINT
    proc.send_signal(stop_signal)
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        print("Pipeline did not exit gracefully after SIGINT, killing it.", file=sys.stderr)
        proc.kill()
        proc.wait()
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
