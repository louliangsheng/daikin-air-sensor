import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM31"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

s = serial.Serial(port, 115200, timeout=0.2)
# RTS 复位让固件重新启动，抓完整 boot 日志
s.dtr = False
s.rts = True
time.sleep(0.15)
s.rts = False

buf = b""
t0 = time.time()
while time.time() - t0 < secs:
    d = s.read(4096)
    if d:
        buf += d
s.close()
sys.stdout.write(buf.decode("utf-8", errors="replace"))
