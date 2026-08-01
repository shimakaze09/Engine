# Generates the bundled placeholder sounds as 16-bit mono WAVs: a footstep
# thump, a pickup chirp, and a soft looping ambient pad for music-stream
# testing. Deterministic output (seeded noise).
import math
import random
import struct
import sys
import wave

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "assets/sounds"
RATE = 22050


def write_wav(path, samples):
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        frames = bytearray()
        for s in samples:
            clamped = max(-1.0, min(1.0, s))
            frames += struct.pack("<h", int(clamped * 32767))
        f.writeframes(bytes(frames))
    print(f"wrote {path} ({len(samples) / RATE:.2f}s)")


def footstep():
    rng = random.Random(41)
    n = int(RATE * 0.09)
    out = []
    low = 0.0
    for i in range(n):
        env = math.exp(-i / (RATE * 0.018))
        noise = rng.uniform(-1.0, 1.0)
        low += 0.25 * (noise - low)
        thump = 0.6 * math.sin(2.0 * math.pi * 70.0 * i / RATE)
        out.append(env * (0.5 * low + 0.5 * thump))
    return out


def pickup():
    n = int(RATE * 0.18)
    out = []
    for i in range(n):
        t = i / RATE
        env = math.exp(-i / (RATE * 0.07))
        freq = 660.0 if t < 0.09 else 990.0
        out.append(0.5 * env * math.sin(2.0 * math.pi * freq * t))
    return out


def ambient():
    n = int(RATE * 8.0)
    chord = [220.0, 277.18, 329.63]
    out = []
    for i in range(n):
        t = i / RATE
        fade = min(t / 0.5, (8.0 - t) / 0.5, 1.0)
        s = sum(math.sin(2.0 * math.pi * f * t) for f in chord)
        tremolo = 0.9 + 0.1 * math.sin(2.0 * math.pi * 0.25 * t)
        out.append(0.12 * fade * tremolo * s / len(chord))
    return out


write_wav(f"{OUT_DIR}/footstep.wav", footstep())
write_wav(f"{OUT_DIR}/pickup.wav", pickup())
write_wav(f"{OUT_DIR}/ambient.wav", ambient())
