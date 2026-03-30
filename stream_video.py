#!/usr/bin/env python3
"""
Stream a video file as MJPEG over TCP to an ESP32 display.

Usage:
    python stream_video.py <video_file> [--port 5000] [--width 320] [--height 240] [--fps 15] [--quality 80]

Each frame is sent as: 4-byte little-endian length + JPEG data.
The video loops when it reaches the end.

Requires: ffmpeg (command-line tool).
"""

import argparse
import socket
import struct
import subprocess
import sys
import time


def mjpeg_frames(path, width, height, fps, quality):
    """Yield individual JPEG frames from a video file using ffmpeg."""
    cmd = [
        "ffmpeg",
        "-stream_loop", "-1",
        "-i", path,
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
    parser = argparse.ArgumentParser(description="Stream MJPEG to ESP32 display")
    parser.add_argument("video", help="Path to video file")
    parser.add_argument("--port", type=int, default=5000, help="TCP port (default: 5000)")
    parser.add_argument("--width", type=int, default=320, help="Display width (default: 320)")
    parser.add_argument("--height", type=int, default=240, help="Display height (default: 240)")
    parser.add_argument("--fps", type=int, default=20, help="Target FPS (default: 20)")
    parser.add_argument("--quality", type=int, default=10,
                        help="JPEG quality 2-31, lower=better (default: 10)")
    args = parser.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)

    print(f"Listening on port {args.port}, waiting for ESP32...")
    print(f"Will stream: {args.video} at {args.width}x{args.height} @ {args.fps}fps, quality={args.quality}")

    while True:
        conn, addr = srv.accept()
        print(f"Client connected: {addr}")
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        # Increase send buffer
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 64 * 1024)

        try:
            frame_count = 0
            total_bytes = 0
            t0 = time.monotonic()

            for frame in mjpeg_frames(args.video, args.width, args.height,
                                      args.fps, args.quality):
                try:
                    # Send 4-byte little-endian length + JPEG data
                    header = struct.pack("<I", len(frame))
                    conn.sendall(header + frame)
                except (BrokenPipeError, ConnectionResetError):
                    print("Client disconnected")
                    break

                frame_count += 1
                total_bytes += len(frame)
                if frame_count % 30 == 0:
                    elapsed = time.monotonic() - t0
                    avg_size = total_bytes / frame_count / 1024
                    print(f"  Sent {frame_count} frames "
                          f"({frame_count/elapsed:.1f} fps avg, "
                          f"{avg_size:.1f} KB/frame avg)")
        except KeyboardInterrupt:
            print("\nStopping...")
            break
        finally:
            conn.close()


if __name__ == "__main__":
    main()
