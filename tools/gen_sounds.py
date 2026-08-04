# Generates the bundled placeholder sounds as 16-bit mono WAVs: the creator
# kit's ~15 effect set (footsteps, jumps, pickups, splashes, UI clicks, win/
# lose jingles, ambient loops) for game authoring without external tools.
# Deterministic output (seeded noise).
import math
import os
import random
import struct
import sys
import wave

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "assets/sounds"
RATE = 22050

# (tmp, final) pairs staged by write_wav and committed atomically at the
# end so an interrupted run can never leave truncated WAVs (audit M-27).
STAGED = []


def write_wav(path, samples):
    with wave.open(path + ".tmp", "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        frames = bytearray()
        for s in samples:
            clamped = max(-1.0, min(1.0, s))
            frames += struct.pack("<h", int(clamped * 32767))
        f.writeframes(bytes(frames))
    STAGED.append((path + ".tmp", path))
    print(f"staged {path} ({len(samples) / RATE:.2f}s)")


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


def jump():
    n = int(RATE * 0.20)
    out = []
    for i in range(n):
        t = i / RATE
        env = math.exp(-i / (RATE * 0.09))
        freq = 180.0 + 900.0 * t
        out.append(0.45 * env * math.sin(2.0 * math.pi * freq * t))
    return out


def land():
    rng = random.Random(7)
    n = int(RATE * 0.16)
    out = []
    low = 0.0
    for i in range(n):
        env = math.exp(-i / (RATE * 0.03))
        noise = rng.uniform(-1.0, 1.0)
        low += 0.18 * (noise - low)
        thump = 0.7 * math.sin(2.0 * math.pi * 52.0 * i / RATE)
        out.append(env * (0.45 * low + 0.55 * thump))
    return out


def splash():
    rng = random.Random(13)
    n = int(RATE * 0.45)
    out = []
    band = 0.0
    prev = 0.0
    for i in range(n):
        t = i / RATE
        env = min(t / 0.02, 1.0) * math.exp(-i / (RATE * 0.12))
        noise = rng.uniform(-1.0, 1.0)
        band += 0.45 * (noise - band)
        high = band - prev
        prev = band
        out.append(0.8 * env * high * 3.0)
    return out


def bounce():
    n = int(RATE * 0.16)
    out = []
    for i in range(n):
        t = i / RATE
        env = math.exp(-i / (RATE * 0.05))
        freq = 340.0 - 180.0 * t
        out.append(0.5 * env * math.sin(2.0 * math.pi * freq * t))
    return out


def click():
    n = int(RATE * 0.05)
    out = []
    for i in range(n):
        t = i / RATE
        env = math.exp(-i / (RATE * 0.008))
        out.append(0.45 * env * math.sin(2.0 * math.pi * 1400.0 * t))
    return out


def whoosh():
    rng = random.Random(29)
    n = int(RATE * 0.30)
    out = []
    low = 0.0
    for i in range(n):
        t = i / RATE
        env = math.sin(math.pi * min(t / 0.30, 1.0))
        noise = rng.uniform(-1.0, 1.0)
        cutoff = 0.08 + 0.30 * env
        low += cutoff * (noise - low)
        out.append(0.6 * env * low)
    return out


def alarm():
    n = int(RATE * 0.5)
    out = []
    for i in range(n):
        t = i / RATE
        freq = 620.0 if (int(t * 8.0) % 2) == 0 else 440.0
        fade = min(t / 0.02, (0.5 - t) / 0.05, 1.0)
        out.append(0.35 * fade * math.sin(2.0 * math.pi * freq * t))
    return out


def jingle(notes, name_rate=0.16, gain=0.4):
    out = []
    for index, freq in enumerate(notes):
        n = int(RATE * name_rate)
        for i in range(n):
            t = i / RATE
            env = math.exp(-i / (RATE * 0.10))
            s = math.sin(2.0 * math.pi * freq * t)
            s += 0.4 * math.sin(2.0 * math.pi * freq * 2.0 * t)
            out.append(gain * env * s / 1.4)
    return out


def win():
    return jingle([523.25, 659.25, 783.99, 1046.50])


def lose():
    return jingle([392.00, 349.23, 311.13, 261.63], name_rate=0.22, gain=0.35)


def chirp():
    n = int(RATE * 0.16)
    out = []
    for i in range(n):
        t = i / RATE
        env = math.sin(math.pi * min(t / 0.16, 1.0))
        freq = 2200.0 + 900.0 * math.sin(2.0 * math.pi * 18.0 * t)
        out.append(0.25 * env * math.sin(2.0 * math.pi * freq * t))
    return out


def wind():
    rng = random.Random(53)
    n = int(RATE * 6.0)
    out = []
    low = 0.0
    gust = 0.0
    for i in range(n):
        t = i / RATE
        fade = min(t / 0.5, (6.0 - t) / 0.5, 1.0)
        noise = rng.uniform(-1.0, 1.0)
        gust = 0.55 + 0.45 * math.sin(2.0 * math.pi * 0.09 * t)
        low += (0.02 + 0.05 * gust) * (noise - low)
        out.append(0.5 * fade * gust * low)
    return out


def waves():
    rng = random.Random(67)
    n = int(RATE * 6.0)
    out = []
    low = 0.0
    for i in range(n):
        t = i / RATE
        fade = min(t / 0.5, (6.0 - t) / 0.5, 1.0)
        swell = 0.25 + 0.75 * (0.5 + 0.5 * math.sin(2.0 * math.pi * t / 3.0))
        noise = rng.uniform(-1.0, 1.0)
        low += (0.03 + 0.09 * swell) * (noise - low)
        out.append(0.55 * fade * swell * low)
    return out


os.makedirs(OUT_DIR, exist_ok=True)
write_wav(f"{OUT_DIR}/footstep.wav", footstep())
write_wav(f"{OUT_DIR}/pickup.wav", pickup())
write_wav(f"{OUT_DIR}/ambient.wav", ambient())
write_wav(f"{OUT_DIR}/jump.wav", jump())
write_wav(f"{OUT_DIR}/land.wav", land())
write_wav(f"{OUT_DIR}/splash.wav", splash())
write_wav(f"{OUT_DIR}/bounce.wav", bounce())
write_wav(f"{OUT_DIR}/click.wav", click())
write_wav(f"{OUT_DIR}/whoosh.wav", whoosh())
write_wav(f"{OUT_DIR}/alarm.wav", alarm())
write_wav(f"{OUT_DIR}/win.wav", win())
write_wav(f"{OUT_DIR}/lose.wav", lose())
write_wav(f"{OUT_DIR}/chirp.wav", chirp())
write_wav(f"{OUT_DIR}/wind.wav", wind())
write_wav(f"{OUT_DIR}/waves.wav", waves())
for tmp_path, final_path in STAGED:
    os.replace(tmp_path, final_path)
print(f"committed {len(STAGED)} sounds")
