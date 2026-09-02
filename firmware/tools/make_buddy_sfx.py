"""Synthesize the Buddy's own 8-bit style sound effects and encode them as
Ogg Opus (16 kHz mono, 60 ms frames) into main/assets/sfx/.

Run: python3 tools/make_buddy_sfx.py   (needs ffmpeg with libopus)
"""
import math, os, struct, subprocess, sys, wave

RATE = 16000
OUT = os.path.join(os.path.dirname(__file__), "..", "main", "assets", "sfx")

NOTE = {"C4": 261.63, "E4": 329.63, "G4": 392.00, "A4": 440.00, "C5": 523.25, "E5": 659.25,
        "G5": 783.99, "A5": 880.00, "B5": 987.77, "C6": 1046.50, "E6": 1318.51}

def tone(freq, ms, wave_type="tri", amp=0.8, attack=0.005, release=0.04):
    n = int(RATE * ms / 1000)
    out = []
    for i in range(n):
        t = i / RATE
        ph = (t * freq) % 1.0
        if wave_type == "sq":
            v = 1.0 if ph < 0.5 else -1.0
        elif wave_type == "tri":
            v = 4 * abs(ph - 0.5) - 1
        else:
            v = math.sin(2 * math.pi * ph)
        env = min(1.0, t / attack) * min(1.0, (n / RATE - t) / release)
        out.append(v * amp * env)
    return out

def gap(ms):
    return [0.0] * int(RATE * ms / 1000)

def seq(*parts):
    s = []
    for p in parts:
        s += p
    return s

CLIPS = {
    # soft two-note chime when the desktop links up
    "buddy_connect":   seq(tone(NOTE["C5"], 110, "tri", 0.6), tone(NOTE["G5"], 200, "tri", 0.6)),
    # rising ping-ping-ping: a prompt needs you
    "buddy_attention": seq(tone(NOTE["E5"], 80, "sq", 0.5), gap(30), tone(NOTE["G5"], 80, "sq", 0.5), gap(30),
                           tone(NOTE["C6"], 180, "sq", 0.5)),
    # three short beeps: still waiting
    "buddy_reminder":  seq(tone(NOTE["A5"], 60, "sq", 0.5), gap(60), tone(NOTE["A5"], 60, "sq", 0.5), gap(60),
                           tone(NOTE["A5"], 60, "sq", 0.5)),
    # major arpeggio up: approved
    "buddy_approve":   seq(tone(NOTE["C5"], 70, "tri"), tone(NOTE["E5"], 70, "tri"), tone(NOTE["G5"], 70, "tri"),
                           tone(NOTE["C6"], 220, "tri")),
    # two falling notes with a buzz: denied
    "buddy_deny":      seq(tone(NOTE["E4"], 140, "sq", 0.55), gap(20), tone(NOTE["C4"], 260, "sq", 0.55, release=0.12)),
    # fanfare: level up
    "buddy_levelup":   seq(tone(NOTE["G5"], 70, "sq", 0.5), gap(20), tone(NOTE["G5"], 70, "sq", 0.5), gap(20),
                           tone(NOTE["G5"], 70, "sq", 0.5), gap(20), tone(NOTE["C6"], 160, "sq", 0.5),
                           tone(NOTE["E6"], 300, "tri", 0.7, release=0.15)),
}

def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, s)) * 32767)) for s in samples))

tmp = sys.argv[1] if len(sys.argv) > 1 else "/tmp"
for name, samples in CLIPS.items():
    wav = os.path.join(tmp, name + ".wav")
    write_wav(wav, samples + gap(40))
    ogg = os.path.join(OUT, name + ".ogg")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", wav, "-c:a", "libopus", "-b:a", "32k",
                    "-ar", "16000", "-ac", "1", "-frame_duration", "60", "-application", "audio", ogg], check=True)
    print(f"{name}: {len(samples)/RATE:.2f} s → {os.path.getsize(ogg)} B")
