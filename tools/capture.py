import serial, time

s = serial.Serial()
s.port = "/dev/ttyACM0"
s.baudrate = 115200
s.timeout = 1
s.open()
s.dtr = False
s.rts = False
out = b""
end = time.time() + 150
while time.time() < end:
    d = s.read(512)
    if d:
        out += d
open("/tmp/panic.log", "wb").write(out)
print("captured", len(out))
