#!/usr/bin/env python3
"""Analyse a rendered piano note: partials (frequency, level, decay) and a spectrogram.

    tools/dpiano/analyse.py note.wav --f1 261.6 [--png out.png] [--partials 12]

Prints, per partial: measured frequency, the inharmonicity B that fits it, level at the
attack, and the decay rate in the first second and later (two-stage decay).
"""

import argparse
import sys
from pathlib import Path

import numpy as np


def read_float_wav(path):
    data = Path(path).read_bytes()
    pos, channels, rate = 12, 2, 48000
    while pos < len(data):
        cid, size = data[pos:pos + 4], int.from_bytes(data[pos + 4:pos + 8], "little")
        if cid == b"fmt ":
            channels = int.from_bytes(data[pos + 10:pos + 12], "little")
            rate = int.from_bytes(data[pos + 12:pos + 16], "little")
        if cid == b"data":
            x = np.frombuffer(data[pos + 8:pos + 8 + size], dtype="<f4").reshape(-1, channels)
            return x.mean(axis=1), rate
        pos += 8 + size
    raise ValueError("no data chunk")


def band_envelope(x, rate, f, width, hop):
    """Level (dB) over time of the band around f, by heterodyning and low-passing."""
    t = np.arange(len(x)) / rate
    z = x * np.exp(-2j * np.pi * f * t)
    n = int(rate / width)
    kernel = np.hanning(n)
    kernel /= kernel.sum()
    env = np.abs(np.convolve(z, kernel, mode="same")) * 2
    return 20 * np.log10(np.maximum(env[::hop], 1e-12))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--f1", type=float, required=True, help="nominal first partial (Hz)")
    ap.add_argument("--partials", type=int, default=12)
    ap.add_argument("--png")
    args = ap.parse_args()

    x, rate = read_float_wav(args.wav)
    n = 1 << 18
    seg = x[: min(len(x), n)]
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n))
    freqs = np.fft.rfftfreq(n, 1 / rate)
    hop = rate // 100

    print(f"{'n':>3} {'Hz':>9} {'B':>9} {'attack dB':>9} {'dB/s 0-1s':>10} {'dB/s 2-4s':>10}")
    f1 = args.f1
    for k in range(1, args.partials + 1):
        guess = k * f1
        if guess > rate * 0.45:
            break
        lo, hi = np.searchsorted(freqs, guess * 0.97), np.searchsorted(freqs, guess * 1.08)
        i = lo + int(np.argmax(spec[lo:hi]))
        # parabolic interpolation
        a, b, c = np.log(spec[i - 1:i + 2] + 1e-20)
        fk = freqs[i] + 0.5 * (a - c) / (a - 2 * b + c) * (freqs[1] - freqs[0])
        bfit = ((fk / (k * f1 / np.sqrt(1 + 0))) ** 2 - 1) / (k * k - 1) if k > 1 else 0.0
        env = band_envelope(x, rate, fk, min(8.0, f1 / 4), hop)
        peak = env[: 50].max()
        s1 = (env[10] - env[100]) / 0.9 if len(env) > 100 else float("nan")
        s2 = (env[200] - env[400]) / 2.0 if len(env) > 400 else float("nan")
        print(f"{k:>3} {fk:9.2f} {bfit:9.2e} {peak:9.1f} {s1:10.1f} {s2:10.1f}")

    if args.png:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(2, 1, figsize=(11, 8))
        ax[0].specgram(x, NFFT=4096, Fs=rate, noverlap=3072, cmap="magma", vmin=-160)
        ax[0].set_ylim(0, min(12000, rate / 2))
        ax[0].set_ylabel("Hz")
        ax[1].plot(freqs, 20 * np.log10(spec / spec.max() + 1e-12), lw=0.6)
        ax[1].set_xscale("log")
        ax[1].set_xlim(20, rate / 2)
        ax[1].set_ylim(-100, 3)
        ax[1].set_xlabel("Hz")
        ax[1].grid(alpha=0.3)
        fig.tight_layout()
        fig.savefig(args.png, dpi=80)


if __name__ == "__main__":
    sys.exit(main())
