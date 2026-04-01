#!/usr/bin/env python3
"""
Broadcast video files as MJPEG over TCP to one or more ESP32 displays.

Usage:
    python stream_video.py [video_dir] [--port 5000] [--width 320] [--height 240] [--fps 30] [--compression 10]

Videos in the directory are pre-encoded to MJPEG AVI at the target resolution
on first run (cached in a .cache/ subfolder), then frames are read and sent
with no real-time transcoding overhead.

Each frame is sent as: 4-byte little-endian length + JPEG data.

Requires: ffmpeg (command-line tool).
"""

import argparse
import pathlib
import socket
import struct
import subprocess
import sys
import threading
import time

VIDEO_EXTS = {".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv", ".wmv", ".m4v"}


def find_videos(directory):
    """Return sorted list of video files in directory."""
    return sorted(
        p for p in pathlib.Path(directory).iterdir()
        if p.is_file() and p.suffix.lower() in VIDEO_EXTS
    )


def pre_encode(src, cache_dir, width, height, fps, quality):
    """Pre-encode a video to MJPEG AVI at target resolution. Returns path."""
    cache_dir.mkdir(exist_ok=True)
    tag = f"{width}x{height}_q{quality}_f{fps}"
    out = cache_dir / f"{src.stem}_{tag}.avi"

    if out.exists() and out.stat().st_mtime >= src.stat().st_mtime:
        return out

    print(f"  Encoding {src.name} → {out.name} ...")
    cmd = [
        "ffmpeg", "-y",
        "-i", str(src),
        "-vf", f"scale={width}:{height}:force_original_aspect_ratio=decrease,"
               f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2",
        "-r", str(fps),
        "-q:v", str(quality),
        "-c:v", "mjpeg",
        "-an",
        "-v", "warning",
        str(out),
    ]
    subprocess.run(cmd, check=True)
    return out


def mjpeg_frames(path):
    """Yield individual JPEG frames from a pre-encoded MJPEG AVI file."""
    cmd = [
        "ffmpeg",
        "-i", str(path),
        "-c:v", "copy",
        "-f", "image2pipe",
        "-v", "warning",
        "pipe:1",
    ]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    try:
        buf = bytearray()
        while True:
            chunk = proc.stdout.read(131072)
            if not chunk:
                # Flush remaining frame
                if buf:
                    soi = buf.find(b"\xff\xd8")
                    eoi = buf.find(b"\xff\xd9", soi + 2) if soi >= 0 else -1
                    if soi >= 0 and eoi >= 0:
                        yield bytes(buf[soi:eoi + 2])
                return
            buf.extend(chunk)

            while True:
                soi = buf.find(b"\xff\xd8")
                if soi < 0:
                    del buf[:max(len(buf) - 1, 0)]
                    break
                eoi = buf.find(b"\xff\xd9", soi + 2)
                if eoi < 0:
                    break
                yield bytes(buf[soi:eoi + 2])
                del buf[:eoi + 2]
    finally:
        proc.terminate()
        proc.wait()


def main():
    parser = argparse.ArgumentParser(description="Broadcast MJPEG to ESP32 displays")
    parser.add_argument("video_dir", nargs="?", default="videos",
                        help="Directory of video files (default: ./videos)")
    parser.add_argument("--port", type=int, default=5000, help="TCP port (default: 5000)")
    parser.add_argument("--width", type=int, default=320, help="Display width (default: 320)")
    parser.add_argument("--height", type=int, default=240, help="Display height (default: 240)")
    parser.add_argument("--fps", type=int, default=30, help="Target FPS (default: 30)")
    parser.add_argument("--compression", type=int, default=10,
                        help="JPEG compression level 2-31, lower=better quality (default: 10)")
    args = parser.parse_args()

    video_dir = pathlib.Path(args.video_dir)
    videos = find_videos(video_dir)
    if not videos:
        print(f"No video files found in {args.video_dir}/")
        sys.exit(1)

    print(f"Found {len(videos)} video(s) in {args.video_dir}/:")
    for v in videos:
        print(f"  {v.name}")

    # Pre-encode all videos
    cache_dir = video_dir / ".cache"
    print(f"\nPre-encoding to {args.width}x{args.height} @ {args.fps}fps, q={args.compression}...")
    encoded = []
    for v in videos:
        enc = pre_encode(v, cache_dir, args.width, args.height, args.fps, args.compression)
        encoded.append(enc)
    print("Pre-encoding done.\n")

    # Thread-safe set of connected clients
    clients = {}  # conn -> addr
    clients_lock = threading.Lock()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(5)

    def accept_loop():
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 128 * 1024)
            conn.settimeout(2.0)
            with clients_lock:
                clients[conn] = addr
            print(f"Client connected: {addr} ({len(clients)} total)")

    acceptor = threading.Thread(target=accept_loop, daemon=True)
    acceptor.start()

    print(f"Listening on port {args.port}, waiting for ESP32(s)...")
    print(f"Settings: {args.width}x{args.height} @ {args.fps}fps, compression={args.compression}")

    while not clients:
        time.sleep(0.1)

    frame_interval = 1.0 / args.fps

    try:
        frame_count = 0
        t0 = time.monotonic()

        while True:
            for i, video in enumerate(videos):
                print(f"Now playing: {videos[i].name}")
                next_frame_time = time.monotonic()

                for frame in mjpeg_frames(encoded[i]):
                    # Pace frames to target FPS
                    now = time.monotonic()
                    sleep_time = next_frame_time - now
                    if sleep_time > 0:
                        time.sleep(sleep_time)
                    next_frame_time += frame_interval

                    packet = struct.pack("<I", len(frame)) + frame
                    dead = []

                    with clients_lock:
                        for conn, addr in clients.items():
                            try:
                                conn.sendall(packet)
                            except (BrokenPipeError, ConnectionResetError, OSError, socket.timeout):
                                dead.append(conn)

                        for conn in dead:
                            addr = clients.pop(conn)
                            conn.close()
                            print(f"Client disconnected: {addr} ({len(clients)} remaining)")

                    frame_count += 1
                    if frame_count % 30 == 0:
                        elapsed = time.monotonic() - t0
                        with clients_lock:
                            n = len(clients)
                        print(f"  {frame_count} frames sent "
                              f"({frame_count/elapsed:.1f} fps avg, "
                              f"{len(frame)/1024:.0f}KB last frame, "
                              f"{n} client(s))")
    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        with clients_lock:
            for conn in clients:
                conn.close()
        srv.close()


if __name__ == "__main__":
    main()
