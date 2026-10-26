"""Bit-level I2C decode of an m2k_cap.py capture, with a PNG plot.

usage: i2c_plot.py <cap.npz> <out.png> [--title T] [--t0 US] [--t1 US]
A SCL high period during which SDA changes is a START/repeated START (SDA
falls) or STOP (SDA rises), not a data bit; data bits are SDA at SCL rise.
"""
import argparse
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument('cap')
ap.add_argument('png')
ap.add_argument('--title', default='')
ap.add_argument('--t0', type=float, default=None, help='window start, us from the first START')
ap.add_argument('--t1', type=float, default=None, help='window end, us from the first START')
a = ap.parse_args()

d = np.load(a.cap)
rate, sda, scl = float(d['rate']), d['sda'].astype(np.int8), d['scl'].astype(np.int8)
# SCL is the line with more edges; swap if the probes were wired the other way round.
if np.count_nonzero(np.diff(sda)) > np.count_nonzero(np.diff(scl)):
    sda, scl = scl, sda
    print('(note: channels swapped - SCL detected on the channel saved as SDA)')
us = 1e6 / rate

# SCL high periods
dscl = np.diff(scl)
rises = np.where(dscl == 1)[0] + 1
falls = np.where(dscl == -1)[0] + 1
events = []  # (sample, kind, value)
for r in rises:
    f = falls[falls > r]
    f = f[0] if len(f) else len(scl)
    seg = sda[r:f]
    ch = np.where(np.diff(seg) != 0)[0]
    if len(ch) == 0:
        events.append((int(r), 'bit', int(seg[0]) if len(seg) else int(sda[r])))
    else:
        for c in ch:
            events.append((int(r + c + 1), 'S' if seg[c + 1] == 0 else 'P', None))
# A START while SCL was already high at the start of the capture
if scl[0] == 1:
    first_rise = rises[0] if len(rises) else len(scl)
    seg = sda[:first_rise]
    for c in np.where(np.diff(seg) != 0)[0]:
        if np.all(scl[:c + 2] == 1):
            events.append((int(c + 1), 'S' if seg[c + 1] == 0 else 'P', None))
events.sort()

# Frame: after a START, bytes of 8 bits + ACK
frames, bits, in_txn, nth, last_cond = [], [], False, 0, None
labels = []  # (sample_start, sample_end, text, color)
conds = []   # (sample, text, color)
for s, kind, v in events:
    if kind == 'bit':
        if in_txn:
            bits.append((s, v))
            if len(bits) == 9:
                byte = 0
                for _, b in bits[:8]:
                    byte = (byte << 1) | b
                ack = 'A' if bits[8][1] == 0 else 'N'
                if nth == 0:
                    txt = f"A{'R' if byte & 1 else 'W'}:{byte >> 1:02X} {ack}"
                    col = '#c060c0' if byte & 1 else '#e09040'
                else:
                    txt = f"{byte:02X} {ack}"
                    col = '#40a060'
                labels.append((bits[0][0], bits[8][0], txt, col))
                frames.append(txt)
                nth += 1
                bits = []
        continue
    if kind == 'S':
        empty = in_txn and nth == 0 and len(bits) == 0 and last_cond is not None
        txt = 'Sr' if in_txn else 'S'
        if empty:
            txt = 'Sr (empty Sr-Sr!)'
        conds.append((s, txt, 'red' if empty else ('#d0a000' if in_txn else '#d04040')))
        frames.append(txt)
        if bits:
            frames.append(f"[{len(bits)} stray bits]")
        in_txn, nth, bits, last_cond = True, 0, [], 'S'
    else:
        conds.append((s, 'P', '#40b040'))
        frames.append('P')
        if bits:
            frames.append(f"[{len(bits)} stray bits]")
        in_txn, nth, bits, last_cond = False, 0, [], 'P'

print(' '.join(frames) if frames else '(no I2C activity)')

# Plot window
first_s = next((s for s, k, _ in events if k == 'S'), 0)
last_e = max([s for s, k, _ in events if k != 'bit'] + [first_s])
t_first = first_s * us
w0 = (first_s * us - 30) if a.t0 is None else t_first + a.t0
w1 = (last_e * us + 30) if a.t1 is None else t_first + a.t1
i0, i1 = max(0, int(w0 / us)), min(len(sda), int(w1 / us) + 1)
t = np.arange(i0, i1) * us - t_first

fig, ax = plt.subplots(figsize=(16, 4.2), dpi=110)
ax.step(t, sda[i0:i1] * 0.8 + 1.5, where='post', color='#e05050', lw=1.2)
ax.step(t, scl[i0:i1] * 0.8 + 0.2, where='post', color='#d0b020', lw=1.2)
ax.text(t[0], 1.9, 'SDA', color='#e05050', va='center', ha='right', fontsize=9)
ax.text(t[0], 0.6, 'SCL', color='#a08000', va='center', ha='right', fontsize=9)
for s0, s1, txt, col in labels:
    if i0 <= s0 <= i1:
        x0, x1 = s0 * us - t_first, s1 * us - t_first
        ax.add_patch(plt.Rectangle((x0, 2.45), x1 - x0, 0.35, color=col, alpha=0.85, lw=0))
        ax.text((x0 + x1) / 2, 2.625, txt, ha='center', va='center', fontsize=8, color='black')
prev_x, level = None, 0
for s, txt, col in conds:
    if i0 <= s <= i1:
        x = s * us - t_first
        # Stagger labels of conditions that sit close together (e.g. an empty Sr-Sr)
        level = (level + 1) % 2 if prev_x is not None and x - prev_x < 40 else 0
        prev_x = x
        ax.axvline(x, color=col, lw=0.8, ls='--', alpha=0.7)
        ax.text(x, 2.9 + 0.22 * level, txt, ha='left' if level else 'center', va='bottom',
                fontsize=8, color=col, fontweight='bold')
ax.set_ylim(0, 3.5)
ax.set_yticks([])
ax.set_xlim(t[0], t[-1])
ax.set_xlabel('time from first START (us)')
title = a.title or a.cap
ax.set_title(f"{title}   [{rate/1e6:g} MS/s]", fontsize=10)
ax.grid(axis='x', alpha=0.25)
fig.tight_layout()
fig.savefig(a.png)
print(f"plot: {a.png}")
