# The HOME Menu jingle: "Pock-et", two soft marimba notes falling a minor third (G5, then E5 with
# a quiet E4 under it), ~1 s with its tail. Replaces build 39's, which sounded like a siren.
# uv run --with numpy cia/make_jingle.py  ->  cia/banner.wav (32 kHz, mono, 16-bit)
import numpy as np, wave, os
SR = 32000
t = np.arange(int(SR * 1.0)) / SR
out = np.zeros_like(t)
def mallet(f, at, amp):
    x = t - at
    on = x >= 0
    x = np.where(on, x, 0)
    env = np.minimum(x / 0.004, 1) * np.exp(-x / 0.32)          # 4 ms onset, a wooden decay
    tone = np.sin(2 * np.pi * f * x) + 0.30 * np.sin(2 * np.pi * f * 3.93 * x) * np.exp(-x / 0.05) \
         + 0.10 * np.sin(2 * np.pi * f * 9.9 * x) * np.exp(-x / 0.015)   # the bar's overtones die fast
    return np.where(on, amp * env * tone, 0)
out += mallet(783.99, 0.00, 1.0)    # pock
out += mallet(659.26, 0.15, 0.95)   # et
out += mallet(329.63, 0.15, 0.30)   # body under it
for d, g in ((0.023, 0.18), (0.041, 0.12), (0.067, 0.08)):   # a small room
    k = int(d * SR)
    out[k:] += g * out[:-k]
fade = np.ones_like(t); n = int(0.15 * SR); fade[-n:] = np.linspace(1, 0, n) ** 2
out *= fade
out *= 0.5 / np.max(np.abs(out))   # -6 dBFS
pcm = (out * 32767).astype('<i2')
p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'banner.wav')
with wave.open(p, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
print(p, len(pcm) / SR, 's')
