"""Relay an Enigma2 channel to a simple_video_player panel.

ffmpeg pulls the channel from the receiver's streaming port, turns it into
panel-sized MJPEG frames plus 16-bit PCM, and the frames go to the panel over
Espressif's udisp protocol (the one simple_video_player's stream_port speaks):
a 16-byte header in front of every JPEG or PCM block.
"""

from __future__ import annotations

import asyncio
from collections import deque
import logging
import os
import struct
import time
from urllib.parse import quote, urlsplit

import aiohttp

from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession

_LOGGER = logging.getLogger(__name__)

_HEADER = struct.Struct("<HBBHHHHI")
UDISP_TYPE_JPG = 3
UDISP_TYPE_PCM = 0x10
UDISP_TYPE_END = 0xFF

HEARTBEAT_S = 2.0
RATE_WAIT_S = 0.5
AUDIO_CHUNK = 4096
AUDIO_QUEUE_MAX = 64
VIDEO_READ = 65536
CHANNEL_CACHE_S = 300
MAX_PAYLOAD = (1 << 22) - 1
SOI = b"\xff\xd8"
EOI = b"\xff\xd9"


def _jpeg_header(width: int, height: int, size: int, frame_id: int) -> bytes:
    return _HEADER.pack(0, UDISP_TYPE_JPG, 0, 0, 0, width, height, (frame_id & 0x3FF) | (size << 10))


def _pcm_header(size: int, channels: int, rate: int) -> bytes:
    return _HEADER.pack(0, UDISP_TYPE_PCM, 0, 0, 0, channels, rate & 0xFFFF, size << 10)


def _heartbeat() -> bytes:
    return _HEADER.pack(0, UDISP_TYPE_END, 0, 0, 0, 0, 0, 0)


class RelaySession:
    """One channel streaming to one panel, until stopped or either end goes away."""

    def __init__(self, relay: SatRelay, stream_url: str, host: str, port: int) -> None:
        self._relay = relay
        self._conf = relay.conf
        self._stream_url = stream_url
        self.host = host
        self.port = port
        self._writer: asyncio.StreamWriter | None = None
        self._proc: asyncio.subprocess.Process | None = None
        self._tasks: list[asyncio.Task] = []
        self._wake = asyncio.Event()
        self._rate_known = asyncio.Event()
        self._rate = 48000
        self._awake = True
        self._frame: bytes | None = None
        self._audio: deque[bytes] = deque(maxlen=AUDIO_QUEUE_MAX)
        self._closing = False

    async def start(self) -> None:
        reader, self._writer = await asyncio.wait_for(asyncio.open_connection(self.host, self.port), 5)
        self._tasks.append(asyncio.create_task(self._read_panel(reader)))
        # The panel says its speaker rate ('A') right after accepting; capture audio at it.
        try:
            await asyncio.wait_for(self._rate_known.wait(), RATE_WAIT_S)
        except TimeoutError:
            pass

        audio_r, audio_w = os.pipe()
        try:
            self._proc = await asyncio.create_subprocess_exec(
                *self._ffmpeg_args(audio_w),
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                pass_fds=(audio_w,),
            )
        finally:
            os.close(audio_w)
        loop = asyncio.get_running_loop()
        audio_reader = asyncio.StreamReader()
        await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(audio_reader), os.fdopen(audio_r, "rb"))

        self._tasks += [
            asyncio.create_task(self._read_video()),
            asyncio.create_task(self._read_audio(audio_reader)),
            asyncio.create_task(self._read_stderr()),
            asyncio.create_task(self._send()),
        ]
        _LOGGER.info("Relaying %s to %s:%d", self._stream_url, self.host, self.port)

    def _ffmpeg_args(self, audio_fd: int) -> list[str]:
        conf = self._conf
        width, height, rotate = conf["width"], conf["height"], conf["rotate"]
        # Scale/pad in the picture's own orientation, then turn it onto the panel.
        pre_w, pre_h = (height, width) if rotate in (90, 270) else (width, height)
        vf = [
            "yadif=deint=interlaced",
            f"scale={pre_w}:{pre_h}:force_original_aspect_ratio=decrease",
            f"pad={pre_w}:{pre_h}:(ow-iw)/2:(oh-ih)/2",
            "setsar=1",
        ]
        if rotate == 90:
            vf.append("transpose=clock")
        elif rotate == 270:
            vf.append("transpose=cclock")
        elif rotate == 180:
            vf.append("hflip,vflip")
        vf.append(f"fps={conf['fps']}")
        return [
            conf["ffmpeg"], "-hide_banner", "-loglevel", "error", "-nostdin",
            "-fflags", "nobuffer", "-flags", "low_delay",
            "-i", self._stream_url,
            "-map", "0:v:0", "-vf", ",".join(vf),
            "-c:v", "mjpeg", "-q:v", str(conf["quality"]), "-pix_fmt", "yuvj420p", "-f", "mjpeg", "pipe:1",
            "-map", "0:a:0", "-ac", "2", "-ar", str(self._rate), "-c:a", "pcm_s16le", "-f", "s16le",
            f"pipe:{audio_fd}",
        ]

    async def _read_panel(self, reader: asyncio.StreamReader) -> None:
        """Messages the panel sends back: C depth, S awake, A rate, T touches, H/K keys."""
        try:
            while True:
                kind = (await reader.readexactly(1))[0]
                if kind == ord("A"):
                    khz, steps = await reader.readexactly(2)
                    self._rate = khz * 1000 + steps * 50
                    self._rate_known.set()
                elif kind == ord("S"):
                    self._awake = (await reader.readexactly(1))[0] != 0
                    if not self._awake:
                        self._frame = None
                        self._audio.clear()
                elif kind in (ord("C"), ord("H")):
                    await reader.readexactly(1)
                elif kind == ord("K"):
                    await reader.readexactly(4)
                elif kind == ord("T"):
                    count = (await reader.readexactly(1))[0]
                    if count:
                        await reader.readexactly(5 * count)
                    if count and self._conf["tap_to_stop"]:
                        _LOGGER.info("Tap on the panel: stopping")
                        self._end()
                        return
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            self._end()

    async def _read_video(self) -> None:
        buf = bytearray()
        stdout = self._proc.stdout
        while True:
            chunk = await stdout.read(VIDEO_READ)
            if not chunk:
                break
            buf += chunk
            while True:
                start = buf.find(SOI)
                if start < 0:
                    del buf[:-1]
                    break
                end = buf.find(EOI, start + 2)
                if end < 0:
                    if start:
                        del buf[:start]
                    break
                frame = bytes(buf[start : end + 2])
                del buf[: end + 2]
                if self._awake and len(frame) <= MAX_PAYLOAD:
                    # Live: only the newest picture is worth sending.
                    self._frame = frame
                    self._wake.set()
        _LOGGER.info("Stream ended")
        self._end()

    async def _read_audio(self, reader: asyncio.StreamReader) -> None:
        while True:
            chunk = await reader.read(AUDIO_CHUNK)
            if not chunk:
                return
            if self._awake:
                self._audio.append(chunk)
                self._wake.set()

    async def _read_stderr(self) -> None:
        while line := await self._proc.stderr.readline():
            _LOGGER.warning("ffmpeg: %s", line.decode(errors="replace").rstrip())

    async def _send(self) -> None:
        width, height = self._conf["width"], self._conf["height"]
        frame_id = 0
        try:
            while True:
                try:
                    await asyncio.wait_for(self._wake.wait(), HEARTBEAT_S)
                except TimeoutError:
                    self._writer.write(_heartbeat())
                    await self._writer.drain()
                    continue
                self._wake.clear()
                while self._audio:
                    chunk = self._audio.popleft()
                    self._writer.write(_pcm_header(len(chunk), 2, self._rate) + chunk)
                if (frame := self._frame) is not None:
                    self._frame = None
                    self._writer.write(_jpeg_header(width, height, len(frame), frame_id) + frame)
                    frame_id += 1
                await self._writer.drain()
        except (ConnectionError, OSError):
            self._end()

    def _end(self) -> None:
        if not self._closing:
            self._closing = True
            asyncio.create_task(self._relay.async_session_ended(self))

    async def stop(self) -> None:
        self._closing = True
        current = asyncio.current_task()
        for task in self._tasks:
            if task is not current:
                task.cancel()
        if self._proc is not None and self._proc.returncode is None:
            self._proc.kill()
            await self._proc.wait()
        if self._writer is not None:
            self._writer.close()
        _LOGGER.info("Relay to %s:%d stopped", self.host, self.port)


class SatRelay:
    """Channel list from OpenWebif and the one running relay session."""

    def __init__(self, hass: HomeAssistant, conf: dict) -> None:
        self.hass = hass
        self.conf = conf
        self._session: RelaySession | None = None
        self._lock = asyncio.Lock()
        self._channels: list[dict[str, str]] = []
        self._channels_at = 0.0
        parts = urlsplit(conf["receiver"])
        self._base = conf["receiver"].rstrip("/")
        self._host = parts.hostname
        self._auth = (
            aiohttp.BasicAuth(conf["username"], conf.get("password", ""))
            if conf.get("username")
            else None
        )

    async def async_channels(self, force: bool = False) -> list[dict[str, str]]:
        if not force and self._channels and time.monotonic() - self._channels_at < CHANNEL_CACHE_S:
            return self._channels
        session = async_get_clientsession(self.hass)
        async with session.get(
            f"{self._base}/api/getallservices", auth=self._auth, timeout=aiohttp.ClientTimeout(total=15)
        ) as resp:
            resp.raise_for_status()
            data = await resp.json(content_type=None)
        bouquet = self.conf.get("bouquet")
        seen: set[str] = set()
        channels: list[dict[str, str]] = []
        for group in data.get("services", []):
            if bouquet and group.get("servicename") != bouquet:
                continue
            for service in group.get("subservices", []):
                ref = service.get("servicereference", "")
                fields = ref.split(":")
                # Markers (flag 64) are separators in a bouquet, not channels.
                if len(fields) < 2 or not fields[1].isdigit() or int(fields[1]) & 64 or ref in seen:
                    continue
                seen.add(ref)
                channels.append({"name": service.get("servicename", ref), "ref": ref})
        self._channels = channels
        self._channels_at = time.monotonic()
        return channels

    def stream_url(self, ref: str) -> str:
        userinfo = ""
        if self.conf.get("username"):
            userinfo = f"{quote(self.conf['username'], safe='')}:{quote(self.conf.get('password', ''), safe='')}@"
        return f"http://{userinfo}{self._host}:{self.conf['stream_port']}/{ref}"

    async def async_play(self, ref: str, host: str, port: int) -> None:
        async with self._lock:
            await self._stop_locked()
            session = RelaySession(self, self.stream_url(ref), host, port)
            self._session = session
            try:
                await session.start()
            except Exception:
                self._session = None
                await session.stop()
                raise

    async def async_stop(self) -> None:
        async with self._lock:
            await self._stop_locked()

    async def _stop_locked(self) -> None:
        if self._session is not None:
            session, self._session = self._session, None
            await session.stop()

    async def async_session_ended(self, session: RelaySession) -> None:
        async with self._lock:
            if self._session is session:
                self._session = None
        await session.stop()
