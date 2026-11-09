# The HOME Menu jingle (cia/banner.wav, 32 kHz mono 16-bit, at most a couple of seconds).
#   uv run --with numpy python cia/make_jingle.py <take> [out.wav]
# takes: rhodes (a soft electric piano, a major-seventh chord rolled upward), tape (a cassette key's
# clack, then a warm chord blooming in), bell (two soft bells, a fifth apart). Build 40's marimba
# "pock-et" is make_jingle.marimba.py; build 39's siren is banner.wav.pre40.
import numpy as np, wave, os, sys
SR = 32000
take = sys.argv[1] if len(sys.argv) > 1 else "stereo"   # 3DStereo's (build 62); "tape" was Pocket's
DUR = {"rhodes": 1.6, "tape": 1.7, "bell": 1.5, "stereo": 1.9}[take]
t = np.arange(int(SR * DUR)) / SR
rng = np.random.default_rng(7)

def at(x0):
    x = t - x0
    return x >= 0, np.maximum(x, 0)

def rhodes(f, x0, amp, tau=0.9):   # two-operator FM: the bark fades into a round sine
    on, x = at(x0)
    idx = 2.2 * np.exp(-x / 0.12) + 0.4
    v = np.sin(2 * np.pi * f * x + idx * np.sin(2 * np.pi * f * x)) * np.minimum(x / 0.003, 1) * np.exp(-x / tau)
    return np.where(on, amp * v, 0)

def pad(f, x0, amp, rise=0.18, tau=0.9):   # a chord tone that blooms in and lingers
    on, x = at(x0)
    env = (1 - np.exp(-x / rise)) * np.exp(-x / tau)
    v = np.sin(2 * np.pi * f * x) + 0.18 * np.sin(2 * np.pi * 2 * f * x) + 0.06 * np.sin(2 * np.pi * 3 * f * x)
    return np.where(on, amp * env * v, 0)

def bell(f, x0, amp):   # a few inharmonic partials, the high ones gone first
    on, x = at(x0)
    v = np.zeros_like(t)
    for r, a, tau in ((1, 1, 1.1), (2.76, .35, .4), (5.4, .15, .15), (8.93, .06, .06)):
        v += a * np.sin(2 * np.pi * f * r * x) * np.exp(-x / tau)
    return np.where(on, amp * np.minimum(x / 0.002, 1) * v, 0)

def clack(x0, amp):   # a cassette key going down: a dull thump and a short plastic tick
    on, x = at(x0)
    thump = np.sin(2 * np.pi * 110 * x) * np.exp(-x / 0.025)
    n = rng.standard_normal(len(t))
    n = np.convolve(n, np.ones(6) / 6, mode="same")
    tick = n * np.exp(-x / 0.006)
    return np.where(on, amp * (0.8 * thump + 0.5 * tick), 0)

out = np.zeros_like(t)
if take == "rhodes":   # Emaj7, rolled up and let ring
    for i, f in enumerate((164.81, 329.63, 415.30, 493.88, 622.25)):
        out += rhodes(f, 0.07 * i, 0.55 if i else 0.4)
elif take == "tape":   # clack, then Dmaj9 blooming in
    out += clack(0.0, 0.9)
    for f in (146.83, 220.0, 293.66, 369.99, 329.63 * 2):
        out += pad(f, 0.09, 0.3)
elif take == "stereo":   # clack, then A in the left ear, E in the right, then Dmaj9 between them
    out += clack(0.0, 0.9)
    left = rhodes(440.0, 0.10, 0.5)
    right = rhodes(659.25, 0.30, 0.45)
    for f in (146.83, 220.0, 293.66, 369.99):
        out += pad(f, 0.50, 0.28)
elif take == "bell":   # A5, then E6 a fifth up, with a low A under the second
    out += bell(880.0, 0.0, 0.8)
    out += bell(1318.51, 0.16, 0.7)
    out += pad(220.0, 0.16, 0.25, rise=0.05, tau=0.6)
st = take == "stereo"
outs = [out + left * 0.92 + right * 0.08, out + right * 0.92 + left * 0.08] if st else [out]   # panned, a touch of bleed
for o in outs:
    for d, g in ((0.029, 0.22), (0.047, 0.15), (0.071, 0.10), (0.103, 0.06)):   # a small warm room
        k = int(d * SR)
        o[k:] += g * o[:-k]
n = int(0.25 * SR)
for o in outs:
    o[-n:] *= np.linspace(1, 0, n) ** 2
pk = max(np.max(np.abs(o)) for o in outs)
outs = [o * 0.5 / pk for o in outs]   # -6 dBFS
p = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "banner.wav")
with wave.open(p, "wb") as w:
    w.setnchannels(len(outs)); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes((np.stack(outs, axis=1) * 32767).astype("<i2").tobytes())
print(take, p, f"{len(out) / SR:.1f} s")
