"""Measures the TuneTest renders: pitch accuracy, vibrato kept, warble, formants, sibilants.
Usage: python3 analyse.py <dir with wavs + truth.csv>"""
import sys, wave, numpy as np

D = sys.argv[1]
SR = 48000
MAJOR = {0, 2, 4, 5, 7, 9, 11}

def load(name):
    with wave.open(f"{D}/{name}") as w:
        raw = w.readframes(w.getnframes())
    b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
    x = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
    x = np.where(x >= 1 << 23, x - (1 << 24), x)
    return x / float(1 << 23)

def yin_track(x, hop=240, win=1200, tmax=700, tmin=40):
    out = []
    for s in range(0, len(x) - win - tmax, hop):
        f = x[s:s + win + tmax]
        if np.sqrt(np.mean(f[:win] ** 2)) < 0.004:
            out.append(np.nan); continue
        a = f[:win]
        d = np.array([np.sum((a - f[t:t + win]) ** 2) for t in range(tmax)])
        c = d.copy(); c[0] = 1
        c[1:] = d[1:] * np.arange(1, tmax) / np.maximum(np.cumsum(d[1:]), 1e-12)
        idx = np.where(c[tmin:] < 0.12)[0]
        if len(idx) == 0:
            out.append(np.nan); continue
        t = idx[0] + tmin
        while t + 1 < tmax and c[t + 1] < c[t]: t += 1
        if 1 <= t < tmax - 1:
            den = c[t - 1] - 2 * c[t] + c[t + 1]
            t = t + (0.5 * (c[t - 1] - c[t + 1]) / den if abs(den) > 1e-9 else 0)
        out.append(69 + 12 * np.log2(SR / t / 440))
    centres = np.arange(len(out)) * hop + win // 2
    return centres, np.array(out)

def lpc_formants(x, order=14, fs_target=12000):
    step = SR // fs_target
    x = x[::step] * np.hamming(len(x[::step]))
    r = np.correlate(x, x, 'full')[len(x) - 1:len(x) + order]
    a = np.zeros(order + 1); a[0] = 1; e = r[0]
    for i in range(1, order + 1):
        k = -(r[i] + np.dot(a[1:i], r[i - 1:0:-1])) / e
        a[1:i] = a[1:i] + k * a[i - 1:0:-1]; a[i] = k; e *= (1 - k * k)
    roots = [z for z in np.roots(a) if np.imag(z) > 0]
    fr = sorted(np.angle(roots) * fs_target / (2 * np.pi))
    bw = [-np.log(abs(z)) * fs_target / np.pi for z in roots]
    return [f for f, b in sorted(zip(np.angle(roots) * fs_target / (2 * np.pi), bw)) if f > 150 and b < 500][:2]

truth = np.loadtxt(f"{D}/truth.csv", delimiter=",", skiprows=1)
tsamp, tmidi, tgap = truth[:, 0], truth[:, 1], truth[:, 2]

# note segments from the truth
segs, start = [], None
for i, m in enumerate(tmidi):
    if m > 0 and start is None: start = i
    if (m <= 0 or i == len(tmidi) - 1) and start is not None:
        segs.append((int(tsamp[start]), int(tsamp[i - 1]))); start = None

def target_for(centre):
    for d in range(7):
        for c in (round(centre) + d, round(centre) - d):
            if c % 12 in MAJOR and abs(c - centre) <= d + 0.5: return c
    return round(centre)

files = ["01_input_untuned.wav", "02_old_engine.wav", "03_new_natural.wav", "04_new_robot.wav",
         "05_new_humanize_flex.wav", "06_new_formant_plus4.wav", "07_new_transparency.wav", "08_new_transpose_plus5.wav",
         "09_harmony_fifth_only.wav", "10_midi_target_D.wav"]
x_in = load(files[0])
tracks = {f: yin_track(load(f)) for f in files}

def smooth(y, n):
    """centred moving average; output aligned with y[n//2 : n//2 + len(valid)]"""
    return np.convolve(y, np.ones(n) / n, 'valid')

def split(y):
    slow = smooth(y, 41)                 # ~200 ms -> note centre
    mid = smooth(y, 5)                   # ~25 ms  -> removes fast warble
    yy = y[20:20 + len(slow)]
    mm = mid[18:18 + len(slow)]
    return yy, mm, slow

print("\n=== PITCH: centre error vs target (cents), vibrato depth (cents rms), warble (cents rms, >12 Hz) ===")
print(f"{'file':28s} {'centre err':>10s} {'per note':>24s} {'vibrato':>8s} {'warble':>7s}  {'voiced %':>8s}")
for f in files:
    c, p = tracks[f]
    errs, vibs, wars, vfr = [], [], [], []
    for si, (a, b) in enumerate(segs):
        settle = 0.85 if si == len(segs) - 1 else 0.45     # last note has a slide until 0.7 s
        sel = (c > a + settle * SR) & (c < b - 0.12 * SR)
        if f is files[0] or True:
            ref = tracks[files[0]][1][sel]
        y = p[sel]
        ok = ~np.isnan(y)
        vfr.append(ok.mean())
        if ok.sum() < 20: continue
        yv = y[ok]
        centre_in = np.nanmean(ref)
        tgt = target_for(centre_in)
        if "transpose" in f: tgt += 5
        if "fifth" in f:       # diatonic 5th above in C major = 4 scale steps
            k = tgt; st = 4
            while st: k += 1; st -= (k % 12 in MAJOR)
            tgt = k
        if "midi_target" in f: tgt = round((centre_in - 2) / 12) * 12 + 2   # nearest D
        errs.append(abs(np.mean(yv) - tgt) * 100)
        if len(yv) < 60: continue
        yy, mm, slow = split(yv)
        vibs.append(np.std(mm - slow) * 100)
        wars.append(np.std(yy - mm) * 100)
    print(f"{f:28s} {np.mean(errs):10.1f} {'/'.join(f'{e:.0f}' for e in errs):>24s} {np.mean(vibs):8.1f} {np.mean(wars):7.2f}  {100*np.mean(vfr):7.0f}%")

def envelope(x, f0):
    """spectral envelope on a log-frequency grid (harmonic peaks joined), 200 Hz - 5 kHz"""
    nfft = 4096
    w = np.hanning(nfft)
    frames = [np.abs(np.fft.rfft(x[i:i + nfft] * w)) for i in range(0, len(x) - nfft, nfft // 4)]
    mag = 20 * np.log10(np.mean(frames, axis=0) + 1e-9)
    freqs = np.fft.rfftfreq(nfft, 1 / SR)
    grid = 200 * 2 ** (np.arange(0, np.log2(5000 / 200), 1 / 48))
    env = []
    for g in grid:
        sel = np.abs(freqs - g) < max(0.6 * f0, 60)
        env.append(mag[sel].max())
    env = np.convolve(env, np.ones(9) / 9, "same")
    return env - env.mean()

def warp(env_in, env_out):
    """how many semitones the output envelope is shifted relative to the input"""
    best, bs = 1e9, 0
    for k in range(-16, 17):   # quarter-tone steps on a 1/48-oct grid -> k*2 bins
        a = env_in[40:-40]; b = np.roll(env_out, -k * 2)[40:-40]
        d = np.mean((a - b) ** 2)
        if d < best: best, bs = d, k / 2
    return bs

print("\n=== FORMANT SHIFT (semitones the spectral envelope moved; 0 = voice character preserved) ===")
print(f"{'note':6s} {'old':>6s} {'new':>6s} {'formant+4':>10s} {'transpose+5':>12s}")
for i, (a, b) in enumerate(segs):
    m = (a + b) // 2
    seg = slice(m - 12000, m + 12000)
    f0 = 440 * 2 ** ((np.nanmean(tracks[files[0]][1][(tracks[files[0]][0] > a) & (tracks[files[0]][0] < b)]) - 69) / 12)
    e_in = envelope(x_in[seg], f0)
    row = [warp(e_in, envelope(load(f)[seg], f0 * (2 ** (5 / 12) if "transpose" in f else 1))) for f in [files[1], files[2], files[5], files[7]]]
    print(f"note {i+1} " + " ".join(f"{v:+6.1f}" if j < 2 else (f"{v:+10.1f}" if j == 2 else f"{v:+12.1f}") for j, v in enumerate(row)))

print("\n=== SIBILANTS / BREATHS: energy ratio out/in in gaps (1.0 = untouched) ===")
gap_s = np.zeros(len(x_in), bool)
for i in range(len(tsamp)):
    if tgap[i] > 0: gap_s[int(tsamp[i]):int(tsamp[i]) + 48] = True
for f in files[1:]:
    y = load(f)
    print(f"{f:28s} {np.sqrt(np.mean(y[gap_s]**2) / np.mean(x_in[gap_s]**2)):.2f}")

y = load(files[6]); n = min(len(y), len(x_in))
lag = np.argmax(np.correlate(y[:n][48000:96000], x_in[:n][48000 - 50:96000 + 50], 'valid')) - 50
print(f"\nTransparency (07, almost no correction): output vs input level ratio "
      f"{np.sqrt(np.mean(y**2)/np.mean(x_in**2)):.3f}, alignment offset {lag} samples")

# plot
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
plt.style.use("dark_background")
fig, axes = plt.subplots(3, 1, figsize=(14, 11), sharex=True)
for ax, (f, col, lab) in zip(axes, [(files[1], "#ff2a6d", "OLD engine"), (files[2], "#00e5ff", "NEW engine - natural (vibrato kept)"),
                                    (files[3], "#ffa21f", "NEW engine - robot (speed 0, vibrato 0)")]):
    c, p = tracks[files[0]]
    ax.plot(c / SR, p, color="#888", lw=1, label="input (sung, out of tune)")
    c2, p2 = tracks[f]
    ax.plot(c2 / SR, p2, color=col, lw=1.4, label=lab)
    for n_ in range(40, 80):
        if n_ % 12 in MAJOR: ax.axhline(n_, color="#ffa21f", lw=0.4, alpha=0.4)
    ax.set_ylabel("pitch (MIDI note)"); ax.legend(loc="upper left"); ax.set_ylim(42, 76)
    ax.set_title(lab + " vs input  —  orange lines = notes of C major")
axes[1].set_xlabel("seconds")
plt.tight_layout(); plt.savefig(f"{D}/pitch_comparison.png", dpi=110)
print("plot written")
