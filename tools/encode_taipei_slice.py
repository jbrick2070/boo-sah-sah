"""Encode the two native Taipei capture directories into review copies."""

import argparse
import json
from pathlib import Path
import subprocess


def checked_take(directory):
    metadata = json.loads((directory / "capture.json").read_text(encoding="utf-8"))
    expected = {
        "complete": True, "width": 2560, "height": 1440, "fps": 30, "frames": 300,
        "cue": "assets/cues/m2_trilogy_180s.json", "scene": "taipei_sanctuary",
        "sample_rate": 48000, "start_sample": 8064000, "last_frame_sample": 8542400,
        "end_sample_exclusive": 8544000,
    }
    for key, value in expected.items():
        if metadata.get(key) != value:
            raise ValueError(f"{directory}: expected {key} = {value!r}")
    checkpoints = metadata["checkpoints"]
    frames = (0, 60, 120, 180, 240, 299)
    if [(p["frame"], p["sample"]) for p in checkpoints] != [(f, 8064000 + 1600 * f) for f in frames]:
        raise ValueError(f"Capture checkpoints are off the 30 fps sample clock in {directory}")
    if len({p["rgb10_fnv1a64"] for p in checkpoints}) != len(frames):
        raise ValueError(f"Repeated image at a motion checkpoint in {directory}")
    raw = directory / "canvas.x2bgr10le"
    if raw.stat().st_size != 2560 * 1440 * 4 * 300:
        raise ValueError(f"Incomplete native capture in {directory}")
    return raw, metadata


def encode(arguments, destination, width, height):
    # A flattened H.264 preview plays in browsers and leaves both native masters intact.
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *arguments,
                    "-an", "-c:v", "libx264", "-threads", "6", "-preset", "fast",
                    "-crf", "22", "-pix_fmt", "yuv420p", "-color_range", "tv",
                    "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "gamma22",
                    "-movflags", "+faststart", str(destination)], check=True)
    result = subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
        "-show_entries", "stream=width,height,avg_frame_rate,nb_read_frames,duration",
        "-of", "json", str(destination)])
    stream = json.loads(result)["streams"][0]
    if (stream["width"], stream["height"], stream["avg_frame_rate"], stream["nb_read_frames"],
            float(stream["duration"])) != (width, height, "30/1", "300", 10.0):
        raise ValueError(f"Invalid encoded stream in {destination}: {stream}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path, help="Directory containing the Phase 1 take")
    parser.add_argument("after", type=Path, help="Directory containing the Taipei street and room take")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    before, after = checked_take(args.before), checked_take(args.after)
    if before[1]["adapter"] != after[1]["adapter"]:
        raise ValueError("The two captures used different adapters")
    args.output.mkdir(parents=True, exist_ok=True)
    raw_inputs = []
    for name, (raw, _) in zip(("before", "after"), (before, after)):
        target = args.output / f"taipei-{name}.mp4"
        source = ["-f", "rawvideo", "-pixel_format", "x2bgr10le", "-video_size", "2560x1440",
                  "-framerate", "30", "-i", str(raw)]
        raw_inputs.extend(source)
        encode([*source, "-vf",
                "scale=in_range=pc:out_range=tv:out_color_matrix=bt709,format=yuv420p"],
               target, 2560, 1440)

    # Keep the labels outside the imagery. Both halves show the full camera frame.
    panel = "scale=1280:720:flags=lanczos:in_range=pc:out_range=tv:out_color_matrix=bt709,format=yuv420p"
    filters = (f"[0:v]{panel}[left];[1:v]{panel}[right];"
              "[left][right]hstack,pad=2560:780:0:60:color=0x10131b,"
              "drawtext=text='Before  /  Phase 1':x=24:y=15:fontsize=28:fontcolor=white,"
              "drawtext=text='After  /  Taipei streets and rooms':x=1304:y=15:fontsize=28:fontcolor=white")
    encode([*raw_inputs, "-filter_complex", filters],
           args.output / "taipei-comparison.mp4", 2560, 780)


if __name__ == "__main__":
    main()
