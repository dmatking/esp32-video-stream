#!/usr/bin/env python3
"""
Broadcast video files as MJPEG over TCP to one or more ESP32 displays.

Usage:
    python stream_video.py [video_dir] [--port 5000] [--width 320] [--height 240] [--fps 15] [--quality 80]

Plays all video files found in the given directory (default: ./videos),
cycling through them in alphabetical order and looping forever.
All connected clients receive the same frames simultaneously.

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
    videos = sorted(
        p for p in pathlib.Path(directory).iterdir()
        if p.is_file() and p.suffix.lower() in VIDEO_EXTS
    )
    return videos


def mjpeg_frames(path, width, height, fps, quality):
    """Yield individual JPEG frames from a video file using ffmpeg."""
    cmd = [
        "ffmpeg",
        "-i", str(path),
        "-vf", f"scale={width}:{height}:force_original_aspect_ratio=decrease,"
               f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2",
        "-r", str(fps),
        "-q:v", str(quality),
        "-f", "mjpeg",
        "-v", "warning",
        "pipe:1",
    ]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    try:
        buf = b""
        while True:
            chunk = proc.stdout.read(4096)
            if not chunk:
                return
            buf += chunk

            # MJPEG frames are delimited by SOI (FFD8) and EOI (FFD9) markers
            while True:
                soi = buf.find(b"\xff\xd8")
                if soi < 0:
                    buf = buf[-1:]  # keep last byte in case of split marker
                    break
                eoi = buf.find(b"\xff\xd9", soi + 2)
                if eoi < 0:
                    break  # need more data
                frame = buf[soi:eoi + 2]
                buf = buf[eoi + 2:]
                yield frame
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
    parser.add_argument("--fps", type=int, default=20, help="Target FPS (default: 20)")
    parser.add_argument("--quality", type=int, default=10,
                        help="JPEG quality 2-31, lower=better (default: 10)")
    args = parser.parse_args()

    videos = find_videos(args.video_dir)
    if not videos:
        print(f"No video files found in {args.video_dir}/")
        sys.exit(1)

    print(f"Found {len(videos)} video(s) in {args.video_dir}/:")
    for v in videos:
        print(f"  {v.name}")

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
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 64 * 1024)
            conn.settimeout(2.0)
            with clients_lock:
                clients[conn] = addr
            print(f"Client connected: {addr} ({len(clients)} total)")

    acceptor = threading.Thread(target=accept_loop, daemon=True)
    acceptor.start()

    print(f"\nListening on port {args.port}, waiting for ESP32(s)...")
    print(f"Settings: {args.width}x{args.height} @ {args.fps}fps, quality={args.quality}")

    # Wait for at least one client before starting
    while not clients:
        time.sleep(0.1)

    try:
        frame_count = 0
        t0 = time.monotonic()

        while True:
            for video in videos:
                print(f"Now playing: {video.name}")
                for frame in mjpeg_frames(video, args.width, args.height,
                                          args.fps, args.quality):
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
