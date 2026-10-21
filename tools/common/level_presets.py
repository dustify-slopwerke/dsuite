#!/usr/bin/env python3
"""Measure (and optionally level-match) factory presets.

Renders each preset with the plugin's renderer (its demo phrase), then reports its loudness as the loudest
400 ms window, K-weighted as in ITU-R BS.1770 (so bass isn't over-counted), plus the sample peak.

    tools/common/level_presets.py [--render build-release/dsynth_render] [--apply] [--target -13] presets/dsynth/*.dsynth
    tools/common/level_presets.py --render build-release/dpiano_render --render-args="--demo mf" --default-volume 0 --max-volume 12 \
        --target -12.5 --apply presets/dpiano/*.dpiano
    tools/common/level_presets.py --render build-release/damp_render --render-args="--guitar demo" --volume-param output_volume \
        --default-volume 0 --max-volume 12 --target -16 --apply presets/damp/*.damp

--apply rewrites the volume parameter (in every layer) so each preset hits the target, while
keeping the peak at or below -1 dBFS.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from scipy.signal import lfilter

RATE = 48000
PEAK_CEILING = -1.0


def read_float_wav(path):
    data = Path(path).read_bytes()
    pos = 12
    fmt_channels = 2
    while pos < len(data):
        chunk, size = data[pos:pos + 4], int.from_bytes(data[pos + 4:pos + 8], "little")
        if chunk == b"fmt ":
            fmt_channels = int.from_bytes(data[pos + 10:pos + 12], "little")
        if chunk == b"data":
            samples = np.frombuffer(data[pos + 8:pos + 8 + size], dtype="<f4")
            return samples.reshape(-1, fmt_channels)
        pos += 8 + size + (size & 1)
    raise ValueError(f"no data chunk in {path}")


def k_weight(x):
    # BS.1770 pre-filter (high shelf) and RLB high-pass, coefficients for 48 kHz
    x = lfilter([1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585], x, axis=0)
    return lfilter([1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621], x, axis=0)


def measure(render, preset, extra):
    with tempfile.NamedTemporaryFile(suffix=".wav") as tmp:
        result = subprocess.run([render, "--preset", str(preset), *extra, "-o", tmp.name], capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"{preset}: render failed: {result.stdout}{result.stderr}")
        audio = read_float_wav(tmp.name)

    peak = 20 * np.log10(max(np.max(np.abs(audio)), 1e-12))
    power = np.sum(k_weight(audio.astype(np.float64)) ** 2, axis=1)
    window, hop = int(0.4 * RATE), int(0.1 * RATE)
    sums = np.convolve(power, np.ones(window), mode="valid")[::hop] / window
    loudness = -0.691 + 10 * np.log10(max(np.max(sums), 1e-12))
    return loudness, peak


def apply_gain(path, delta, default_volume, max_volume, param):
    text = Path(path).read_text()
    sections = re.split(r"(?im)^(\[layer b\]\s*)$", text)
    # sections: [main, header, layer b body] or [main]
    out = []
    for i, body in enumerate(sections):
        if i % 2 == 1:
            out.append(body)
            continue
        match = re.search(rf"(?m)^{param}[ \t]*=[ \t]*(-?[\d.]+)[ \t]*$", body)
        if match:
            value = max(-48.0, min(max_volume, float(match.group(1)) + delta))
            body = body[:match.start()] + f"{param} = {value:g}" + body[match.end():]
        else:
            value = max(-48.0, min(max_volume, default_volume + delta))
            body = body.rstrip("\n") + f"\n{param} = {value:g}\n" + ("\n" if i + 1 < len(sections) else "")
        out.append(body)
    Path(path).write_text("".join(out))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("presets", nargs="+")
    parser.add_argument("--render", default="build-release/dsynth_render")
    parser.add_argument("--target", type=float, default=-13.0, help="loudest-window loudness, LUFS")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--max-volume", type=float, default=6.0, help="the plugin's master_volume maximum (dsynth 6, dpiano 12)")
    parser.add_argument("--volume-param", default="master_volume", help="the preset id to adjust (damp: output_volume)")
    parser.add_argument("--render-args", default="", help="extra renderer arguments, e.g. --guitar demo for damp")
    parser.add_argument("--default-volume", type=float, default=-6.0,
                        help="the plugin's master_volume default, for presets that don't set it (dsynth -6, dpiano 0)")
    args = parser.parse_args()

    extra = args.render_args.split()
    for preset in args.presets:
        loudness, peak = measure(args.render, preset, extra)
        delta = min(args.target - loudness, PEAK_CEILING - peak)
        flag = "" if abs(delta) < 0.75 else f"  (off by {-delta:+.1f} dB)"
        print(f"{Path(preset).name:40s} loud {loudness:6.1f} LUFS  peak {peak:5.1f} dBFS{flag}")
        if args.apply and abs(delta) >= 0.25:
            apply_gain(preset, round(delta, 1), args.default_volume, args.max_volume, args.volume_param)
            loudness, peak = measure(args.render, preset, extra)
            print(f"{'':40s} -> {loudness:6.1f} LUFS  peak {peak:5.1f} dBFS")


if __name__ == "__main__":
    sys.exit(main())
