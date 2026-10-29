"""Small original chiptune loop + device beeps, synced to the video timeline."""
import sys, wave
import numpy as np

SR = 44100
DUR = float(sys.argv[1])           # seconds
BEEPS = [float(x) for x in sys.argv[2:]]
BPM = 108
BEAT = 60 / BPM
S16 = BEAT / 4

n = int(DUR * SR)
mix = np.zeros(n)


def note(midi):
    return 440 * 2 ** ((midi - 69) / 12)


def env(length, a=0.004, r=0.08):
    t = np.arange(length) / SR
    e = np.minimum(1, t / a) * np.exp(-t / r)
    return e


def put(sig, t0, gain):
    i = int(t0 * SR)
    if i >= n:
        return
    sig = sig[: n - i]
    mix[i:i + len(sig)] += sig * gain


def pulse(f, length, duty=0.25):
    t = np.arange(length) / SR
    return np.where((t * f) % 1 < duty, 1.0, -1.0)


def tri(f, length):
    t = np.arange(length) / SR
    return 2 * np.abs(2 * ((t * f) % 1) - 1) - 1


# C  Am  F  G  (two bars each chord = one bar here)
CHORDS = [(60, 64, 67, 72), (57, 60, 64, 69), (53, 57, 60, 65), (55, 59, 62, 67)]
BASS = [36, 33, 29, 31]
PATTERN = [0, 1, 2, 3, 2, 1, 2, 3, 0, 1, 2, 3, 2, 3, 2, 1]
MELODY = [  # (16th index in 4-bar phrase, midi, length in 16ths)
    (0, 76, 3), (4, 79, 2), (6, 76, 2), (8, 74, 4), (12, 72, 4),
    (16, 72, 3), (20, 76, 2), (22, 74, 2), (24, 72, 6), (30, 69, 2),
    (32, 69, 3), (36, 72, 2), (38, 74, 2), (40, 76, 4), (44, 77, 4),
    (48, 79, 6), (54, 77, 2), (56, 76, 4), (60, 74, 4),
]

intro_end = 3.4            # just the arpeggio during the title card
bar = 16 * S16
t = 0.0
k = 0
while t < DUR:
    chord = CHORDS[k % 4]
    full = t >= intro_end
    for s in range(16):
        ts = t + s * S16
        L = int(S16 * SR * 0.95)
        f = note(chord[PATTERN[s]] + 12)
        put(pulse(f, L, 0.125) * env(L, r=0.06), ts, 0.045)
        if full and s % 4 == 0:
            Lb = int(BEAT * SR * 0.9)
            put(tri(note(BASS[k % 4]), Lb) * env(Lb, a=0.002, r=0.35), ts, 0.16)
        if full and s % 4 == 2:
            Lh = int(0.04 * SR)
            put(np.random.uniform(-1, 1, Lh) * env(Lh, a=0.001, r=0.012), ts, 0.04)
    if full:
        phrase_t = t - bar * (k % 4)
        for idx, m, ln in MELODY:
            if idx // 16 == k % 4:
                ts = phrase_t + idx * S16
                L = int(ln * S16 * SR * 0.9)
                put(pulse(note(m), L, 0.5) * env(L, a=0.006, r=0.5), ts, 0.035)
    t += bar
    k += 1

# soften: one-pole lowpass
y = np.zeros_like(mix)
a = 0.35
acc = 0.0
for i in range(0, n, SR):
    seg = mix[i:i + SR]
    out = np.empty_like(seg)
    for j, v in enumerate(seg):
        acc += a * (v - acc)
        out[j] = acc
    y[i:i + SR] = out
mix = y

# fades
fi, fo = int(0.8 * SR), int(2.0 * SR)
mix[:fi] *= np.linspace(0, 1, fi)
mix[-fo:] *= np.linspace(1, 0, fo)

# device beep: 1800 Hz square, 120 ms (same as firmware), duck music around it
for b in BEEPS:
    L = int(0.12 * SR)
    i = int(b * SR)
    duck = int(0.35 * SR)
    mix[i:i + duck] *= 0.45
    put(pulse(1800, L, 0.5) * np.minimum(1, np.arange(L)[::-1] / 200), b, 0.12)

mix = np.clip(mix, -1, 1)
pcm = (mix * 32767).astype(np.int16)
with wave.open("music.wav", "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(pcm.tobytes())
print("ok", DUR)
