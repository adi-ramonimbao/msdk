import sys, time, serial
# console_cap.py <port> <timeout_s> <logfile>: echo the console until "Example Succeeded/Failed" or timeout.
s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
end = time.time() + float(sys.argv[2])
buf = b''
with open(sys.argv[3], 'wb') as log:
    while time.time() < end:
        d = s.read(256)
        if not d:
            continue
        log.write(d); log.flush()
        buf += d
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            text = line.decode(errors='replace').rstrip('\r')
            print(text, flush=True)
            if text.startswith("=== Summary") or text.startswith("Example Succeeded") or text.startswith("Example Failed"):
                sys.exit(0)
print('(timeout waiting for summary)')
