#!/usr/bin/env python3
"""Pack individual media frame files into a single binary blob.

Supports H.264/H.265 video (frame-NNNN.h264/h265, 1-based) and
Opus audio (sample-NNN.opus, 0-based).

Blob format (all little-endian):
    [uint32  frame_count]
    [frame_count × {uint32 offset, uint32 len}]   -- index table (offset relative to data start)
    [frame data bytes ...]                          -- frame payloads, tightly packed

Usage:
    python pack_frames.py <frame_dir> <count> <output.blob>

Pass count=0 to auto-detect the number of frames in the directory.

Examples:
    python pack_frames.py examples/sample_data/h264SampleFrames 1500 video.blob
    python pack_frames.py examples/sample_data/opusSampleFrames 619 audio.blob
    python pack_frames.py examples/sample_data/h264SampleFrames 0 video.blob   # auto-detect
"""

import struct
import sys
from pathlib import Path


def count_frames(frame_dir):
    """Auto-detect frame count by scanning directory."""
    opus = sorted(frame_dir.glob("sample-*.opus"))
    if opus:
        return len(opus)
    h264 = sorted(frame_dir.glob("frame-*.h264"))
    if h264:
        return len(h264)
    h265 = sorted(frame_dir.glob("frame-*.h265"))
    if h265:
        return len(h265)
    return 0


def detect_and_read_frames(frame_dir, count):
    """Auto-detect frame format and read all frames."""
    # Try Opus first (0-based: sample-NNN.opus)
    test_opus = frame_dir / "sample-000.opus"
    if test_opus.exists():
        frames = []
        for i in range(count):
            path = frame_dir / f"sample-{i:03d}.opus"
            if not path.exists():
                print(f"Error: {path} not found")
                sys.exit(1)
            frames.append(path.read_bytes())
        return frames

    # Try H.264/H.265 (1-based: frame-NNNN.h264/h265)
    frames = []
    for i in range(1, count + 1):
        path = frame_dir / f"frame-{i:04d}.h264"
        if not path.exists():
            path = frame_dir / f"frame-{i:04d}.h265"
        if not path.exists():
            print(f"Error: frame {i} not found in {frame_dir}")
            sys.exit(1)
        frames.append(path.read_bytes())
    return frames


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <frame_dir> <count> <output.blob>")
        sys.exit(1)

    frame_dir = Path(sys.argv[1])
    count = int(sys.argv[2])
    output = Path(sys.argv[3])

    if not frame_dir.is_dir():
        print(f"Error: {frame_dir} is not a directory")
        sys.exit(1)

    if count == 0:
        count = count_frames(frame_dir)
        if count == 0:
            print(f"Error: no frame files found in {frame_dir}")
            sys.exit(1)
        print(f"Auto-detected {count} frames")

    frames = detect_and_read_frames(frame_dir, count)

    # Build index table and data blob
    header_size = 4 + count * 8  # frame_count + index table
    offset = 0
    index = []
    data = bytearray()
    for frame in frames:
        index.append((offset, len(frame)))
        data.extend(frame)
        offset += len(frame)

    # Write output
    output.parent.mkdir(parents=True, exist_ok=True)
    with open(output, "wb") as f:
        f.write(struct.pack("<I", count))
        for off, length in index:
            f.write(struct.pack("<II", off, length))
        f.write(data)

    total_size = header_size + len(data)
    print(f"Packed {count} frames -> {output} ({total_size} bytes, {total_size/1024/1024:.1f} MB)")


if __name__ == "__main__":
    main()
