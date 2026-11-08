#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
ViceVersa's sampled wave banks, resynthesised from recordings, as the Prophet
VS's instrument waves were: the harmonic spectrum of single cycles taken
through a note, from its attack to its sustain.

    tools/extract_waves.py <OLPC sound samples folder> ViceVersa/Dsp/SampledBanks.hpp

The recordings are the One Laptop Per Child sound sample library
(https://wiki.laptop.org/go/Sound_samples), CC-BY 3.0; the generated header
credits them. Only spectra are kept: no audio is copied.

Per sample: the pitch is tracked, frame times are spread over the voiced part
of the note (denser near the attack), and at each the period is refined and
six periods are projected on the harmonics, Hann windowed. Within a bank all
frames take the phases of its loudest frame, so that crossfading two frames
never cancels a harmonic: only the magnitudes move. A sample whose harmonics
hold too little of its energy (not one steady pitch) is refused.
"""

import sys
import numpy as np
import scipy.io.wavfile as wavfile
import warnings

warnings.filterwarnings("ignore", category=wavfile.WavFileWarning)

HARMONICS = 96
FRAMES = 16

# Bank -> [(sample, frames taken from it)], the frames summing to FRAMES
BANKS = {
    "Brass": [("trumpet.wav", 6), ("trombone.wav", 5), ("tuba.wav", 5)],
    "Reeds": [("clarinette.wav", 4), ("sax1.wav", 4), ("sax3.wav", 4), ("harmonica1.wav", 2),
              ("thaikhean4.wav", 2)],
    "Strings": [("violin.wav", 4), ("violabowed.wav", 4), ("cello1.wav", 4), ("cello2.wav", 4)],
    "Voices": [("voix1.wav", 4), ("voix2.wav", 4), ("singah1.wav", 3), ("singsynthooh.wav", 3),
               ("singsynthouah.wav", 2)],
    "Keys": [("piano1.wav", 4), ("epiano1.wav", 4), ("organ1.wav", 2), ("organ3.wav", 2),
             ("clavichord.wav", 2), ("toypiano1.wav", 2)],
    "Plucked": [("acguit.wav", 4), ("banjo.wav", 3), ("koto2.wav", 3), ("harp.wav", 3),
                ("sitar.wav", 3)],
    "Winds": [("flute.wav", 4), ("flute2.wav", 3), ("shakuhachi.wav", 4), ("ocarina1.wav", 3),
              ("didjeridu1.wav", 2)],
    "Mallets": [("marimba1.wav", 4), ("vibe1.wav", 4), ("kalimba1.wav", 4), ("toypiano2.wav", 4)],
}

MIN_HARMONICITY = 0.55


def load(path):
    sr, x = wavfile.read(path)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    x /= max(1e-9, np.max(np.abs(x)))
    return sr, x


def f0_at(x, sr, centre, lo=45.0, hi=1500.0, length=4096):
    """Normalised autocorrelation, parabolic peak: (f0, clarity)"""
    a = max(0, centre - length // 2)
    seg = x[a:a + length]
    if len(seg) < length:
        return None, 0.0
    seg = seg - seg.mean()
    min_lag, max_lag = int(sr / hi), min(int(sr / lo), length // 2)
    best, best_lag = -1.0, None
    energy = np.dot(seg, seg)
    if energy <= 1e-9:
        return None, 0.0
    r = np.correlate(seg, seg, "full")[length - 1:]
    # Difference function, cumulative-mean normalised (YIN)
    d = r[0] + np.array([np.dot(seg[k:], seg[k:]) for k in range(max_lag + 2)]) - 2 * r[:max_lag + 2]
    cmnd = np.ones_like(d)
    running = np.cumsum(d[1:])
    cmnd[1:] = d[1:] * np.arange(1, len(d)) / np.maximum(running, 1e-12)
    for lag in range(min_lag, max_lag):
        if cmnd[lag] < 0.15 and cmnd[lag] <= cmnd[lag + 1]:
            best_lag = lag
            break
    if best_lag is None:
        best_lag = min_lag + int(np.argmin(cmnd[min_lag:max_lag]))
    a0, b0, c0 = cmnd[best_lag - 1], cmnd[best_lag], cmnd[best_lag + 1]
    den = a0 - 2 * b0 + c0
    shift = 0.5 * (a0 - c0) / den if abs(den) > 1e-12 else 0.0
    return sr / (best_lag + shift), 1.0 - float(b0)


def spectrum_at(x, sr, centre, f0):
    """Harmonics 1..HARMONICS at `centre`, over six periods, and the share of
    the window's energy they hold"""
    period = sr / f0
    n = int(round(6 * period))
    a = int(centre - n // 2)
    if a < 0 or a + n > len(x):
        return None, 0.0
    seg = x[a:a + n] - x[a:a + n].mean()
    w = np.hanning(n)
    t = np.arange(n)
    c = np.zeros(HARMONICS, dtype=np.complex128)
    for h in range(1, HARMONICS + 1):
        if h * f0 >= 0.95 * sr / 2:
            break
        c[h - 1] = 2.0 * np.sum(seg * w * np.exp(-2j * np.pi * h * f0 * t / sr)) / np.sum(w)
    # Energy the harmonics explain: resynthesise and compare
    rec = np.zeros(n)
    for h in range(1, HARMONICS + 1):
        if c[h - 1] != 0:
            rec += np.real(c[h - 1] * np.exp(2j * np.pi * h * f0 * t / sr))
    e_in = np.sum((seg * w) ** 2)
    e_res = np.sum(((seg - rec) * w) ** 2)
    harmonicity = 1.0 - e_res / max(e_in, 1e-12)
    # Fundamental at phase 0: the cycle starts at the same point in every frame
    phi = np.angle(c[0])
    c *= np.exp(-1j * phi * np.arange(1, HARMONICS + 1))
    return c, harmonicity


def frames_of(path, count):
    sr, x = load(path)
    hop = 512
    rms = np.array([np.sqrt(np.mean(x[i:i + 2048] ** 2)) for i in range(0, len(x) - 2048, hop)])
    if len(rms) == 0:
        raise RuntimeError("too short")
    loud = np.where(rms > 0.15 * rms.max())[0]
    start, end = loud[0] * hop + 2048, loud[-1] * hop
    # The pitch where the note is: the median of the clear estimates
    est = [f0_at(x, sr, c) for c in range(start, end, 2048)]
    f0s = [f for f, clarity in est if f and clarity > 0.6]
    if not f0s:
        raise RuntimeError("no steady pitch")
    f0med = float(np.median(f0s))
    out = []
    for j in range(count):
        # Denser near the attack
        u = (j + 0.5) / count
        centre = int(start + (end - start) * u ** 1.6)
        f0, clarity = f0_at(x, sr, centre)
        if not f0 or abs(np.log2(f0 / f0med)) > 0.08:
            f0 = f0med
        c, harmonicity = spectrum_at(x, sr, centre, f0)
        if c is None:
            raise RuntimeError("frame out of the sample")
        out.append((c, harmonicity, f0))
    return out


def literal(v):
    """A C++ float literal: 0f is not one"""
    t = f"{v:.4g}"
    if "." not in t and "e" not in t and "inf" not in t and "nan" not in t:
        t += ".0"
    return t + "f"


def main(root, header):
    banks = {}
    report = []
    for name, sources in BANKS.items():
        frames = []
        for sample, count in sources:
            got = frames_of(f"{root}/tam44all/{sample}", count)
            h = np.mean([g[1] for g in got])
            report.append(f"{name:8} {sample:20} f0 {np.median([g[2] for g in got]):7.1f} Hz  "
                          f"harmonicity {h:.2f}")
            if h < MIN_HARMONICITY:
                raise RuntimeError(f"{sample}: harmonicity {h:.2f}, not one steady pitch")
            frames += [g[0] for g in got]
        assert len(frames) == FRAMES, (name, len(frames))
        # The phases of the loudest frame, for all
        loudest = max(range(FRAMES), key=lambda i: np.sum(np.abs(frames[i]) ** 2))
        phase = np.angle(frames[loudest])
        mags = np.array([np.abs(f) for f in frames])
        mags /= max(1e-12, mags.max())
        banks[name] = (mags, phase)
    print("\n".join(report))

    with open(header, "w") as f:
        f.write("#pragma once\n\n/* SPDX-License-Identifier: GPL-3.0-or-later */\n\n")
        f.write("/* Generated by tools/extract_waves.py: do not edit.\n *\n")
        f.write(" * Harmonic spectra resynthesised from recordings of the One Laptop Per\n"
                " * Child sound sample library, https://wiki.laptop.org/go/Sound_samples,\n"
                " * licensed CC-BY 3.0 (https://creativecommons.org/licenses/by/3.0/),\n"
                " * donated to Dr. Richard Boulanger (cSounds.com) for OLPC. Only spectra\n"
                " * are kept. Per bank, the samples (tam44all/) and frames taken:\n")
        for name, sources in BANKS.items():
            f.write(f" *   {name}: " + ", ".join(f"{s} ({n})" for s, n in sources) + "\n")
        f.write(" */\n\n#include <array>\n\nnamespace viceversa::sampled\n{\n")
        f.write(f"inline constexpr int kHarmonics = {HARMONICS};\n")
        f.write(f"inline constexpr int kFrames = {FRAMES};\n\n")
        f.write("struct Bank\n{\n  //! Magnitudes, [frame][harmonic], the loudest 1\n"
                "  std::array<std::array<float, kHarmonics>, kFrames> magnitude;\n"
                "  //! One phase per harmonic, shared by every frame\n"
                "  std::array<float, kHarmonics> phase;\n};\n\n")
        for name, (mags, phase) in banks.items():
            f.write(f"inline constexpr Bank k{name}{{\n  {{{{\n")
            for fr in mags:
                f.write("    {" + ", ".join(literal(v) for v in fr) + "},\n")
            f.write("  }},\n  {" + ", ".join(literal(v) for v in phase) + "}};\n\n")
        f.write("}\n")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
