"""Capture SDA/SCL with the ADALM2000 logic analyzer over libiio.

usage: m2k_cap.py <out.npz> [--rate HZ] [--samples N] [--pretrig N]
                  [--trigger start|sda-fall|none] [--sda 0] [--scl 1]
                  [--timeout-ms MS] [--uri ip:192.168.2.1]
"""
import argparse
import numpy as np
import iio

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--uri', default='ip:192.168.2.1')
ap.add_argument('--rate', type=float, default=4e6)
ap.add_argument('--samples', type=int, default=16384)
ap.add_argument('--pretrig', type=int, default=1024)
ap.add_argument('--trigger', choices=['start', 'sda-fall', 'none'], default='start')
ap.add_argument('--sda', type=int, default=0)
ap.add_argument('--scl', type=int, default=1)
ap.add_argument('--timeout-ms', type=int, default=10000)
a = ap.parse_args()

ctx = iio.Context(a.uri)
ctx.set_timeout(a.timeout_ms)
la = ctx.find_device('m2k-logic-analyzer')
rx = ctx.find_device('m2k-logic-analyzer-rx')

for i in range(16):
    la.find_channel(f'voltage{i}', False).attrs['direction'].value = 'in'

rx.attrs['sampling_frequency'].value = str(int(a.rate))
rate = float(rx.attrs['sampling_frequency'].value)

chans = [rx.find_channel(f'voltage{i}', False) for i in range(16)]
for ch in chans:
    ch.attrs['trigger'].value = 'none'
if a.trigger == 'start':
    chans[a.sda].attrs['trigger'].value = 'edge-falling'
    chans[a.scl].attrs['trigger'].value = 'level-high'
    chans[0].attrs['trigger_logic_mode'].value = 'and'
elif a.trigger == 'sda-fall':
    chans[a.sda].attrs['trigger'].value = 'edge-falling'
    chans[0].attrs['trigger_logic_mode'].value = 'or'
chans[0].attrs['trigger_mux_out'].value = 'trigger-logic'
chans[0].attrs['trigger_delay'].value = str(-a.pretrig if a.trigger != 'none' else 0)

for ch in chans:
    ch.enabled = True
buf = iio.Buffer(rx, a.samples, False)
buf.refill()
words = np.frombuffer(buf.read(), dtype=np.uint16)
del buf

sda = ((words >> a.sda) & 1).astype(np.uint8)
scl = ((words >> a.scl) & 1).astype(np.uint8)
np.savez_compressed(a.out, rate=rate, sda=sda, scl=scl,
                    pretrig=a.pretrig if a.trigger != 'none' else 0)
print(f"captured {len(words)} samples at {rate/1e6:g} MS/s "
      f"({len(words)/rate*1e3:.2f} ms), trigger={a.trigger}")
