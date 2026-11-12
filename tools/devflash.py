#!/usr/bin/env python3
"""devflash - push an app-only image to a Vandeta board without esptool.

Two transports:

  framed (default over USB)  A5 5A frames, FR_OTA_BEGIN/DATA/END. 2 KB per
                             chunk, so a ~4.9 MB image takes a few seconds.
  text   (--uart)            `devflash begin/chunk/end` with hex chunks. Works
                             on a plain UART where the framed protocol does not
                             exist; slower because every chunk is a line.

Both paths are resumable at the chunk level: if the transfer stops, the board
still boots the old image (the OTA handle writes the inactive partition and the
boot pointer moves only in `end` after the crc32 matches). Re-running starts a
fresh session.

  python3 devflash.py --port /dev/ttyACM0 --image vandeta-app-lilygo-t-embed-cc1101-dev.bin
  python3 devflash.py --port /dev/ttyACM0 --image app.bin --uart --chunk 256
"""
import argparse
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

MAGIC = b"\xa5\x5a"
FR_OTA_BEGIN = 0x0C
FR_OTA_DATA = 0x0D
FR_OTA_END = 0x0E
FR_OTA_ABORT = 0x0F
FR_OTA_R = 0x8C
FR_SET_MODE = 0x09


def send(port, ftype, payload=b""):
    head = MAGIC + bytes([ftype]) + len(payload).to_bytes(2, "little")
    crc = zlib.crc32(head[2:] + payload) & 0xFFFF
    port.write(head + payload + crc.to_bytes(2, "little"))


def read_frame(port, timeout=10.0):
    """Read one A5 5A frame; returns (type, payload) or (None, b'')."""
    deadline = time.time() + timeout
    state = 0
    head = b""
    while time.time() < deadline:
        b = port.read(1)
        if not b:
            continue
        if state == 0:
            state = 1 if b == MAGIC[0:1] else 0
        elif state == 1:
            if b == MAGIC[1:2]:
                state = 2
            else:
                state = 1 if b == MAGIC[0:1] else 0
        elif state == 2:
            head = b
            state = 3
        elif state == 3:
            head += b
            state = 4
        elif state == 4:
            head += b
            ln = int.from_bytes(head[1:3], "little")
            payload = b""
            while len(payload) < ln and time.time() < deadline:
                chunk = port.read(ln - len(payload))
                if chunk:
                    payload += chunk
            tail = b""
            while len(tail) < 2 and time.time() < deadline:
                tail += port.read(2 - len(tail))
            return head[0], payload
    return None, b""


def read_text(port, timeout=8.0):
    deadline = time.time() + timeout
    out = b""
    while time.time() < deadline:
        b = port.read(4096)
        if b:
            out += b
            if b"# " in out:
                break
    return out.decode(errors="replace")


def enter_binary(port):
    port.write(b"proto binary\n")
    deadline = time.time() + 8
    buf = b""
    while time.time() < deadline:
        b = port.read(1024)
        if b:
            buf += b
            if b"binary protocol ON" in buf:
                return True
    # Maybe the port was already framed (a previous session left it there).
    try:
        send(port, 0x01)  # PING
        t, p = read_frame(port, 3)
        return t is not None
    except Exception:
        return False


def leave_binary(port):
    try:
        send(port, FR_SET_MODE, bytes([0]))
        read_frame(port, 3)
    except Exception:
        pass


def device_build_id(port):
    """`patch list` prints `build <16 hex>`: the 8-byte folded ID."""
    port.reset_input_buffer()
    port.write(b"patch list\n")
    txt = read_text(port, 8)
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith("build "):
            return bytes.fromhex(line[6:].strip())
    return None


def upload_framed(port, image, build_id, chunk=2048, force=False):
    crc = zlib.crc32(image) & 0xFFFFFFFF
    ident = build_id or b"\x00" * 8
    flags = 1 if force else 0
    send(port, FR_OTA_BEGIN, len(image).to_bytes(4, "little") + crc.to_bytes(4, "little") + ident + bytes([flags]))
    t, p = read_frame(port)
    msg = p.decode(errors="replace")
    print(msg)
    if t is None or not msg.startswith("OK"):
        return False

    sent = 0
    t0 = time.time()
    while sent < len(image):
        piece = image[sent:sent + chunk]
        send(port, FR_OTA_DATA, sent.to_bytes(4, "little") + piece)
        t, p = read_frame(port)
        msg = p.decode(errors="replace")
        if t is None or not msg.startswith("OK"):
            print("chunk failed at %d: %s" % (sent, msg))
            return False
        sent += len(piece)
        if sent % (chunk * 64) == 0 or sent == len(image):
            pct = 100.0 * sent / len(image)
            print("\r  %d/%d bytes (%.1f%%)" % (sent, len(image), pct), end="", flush=True)
    dt = max(1e-3, time.time() - t0)
    print("\n  transport: %.1f s, %.0f KB/s" % (dt, len(image) / 1024.0 / dt))

    send(port, FR_OTA_END)
    t, p = read_frame(port, 30)
    msg = p.decode(errors="replace")
    print(msg)
    return t is not None and msg.startswith("OK")


def upload_text(port, image, build_id, chunk=256, force=False):
    crc = zlib.crc32(image) & 0xFFFFFFFF
    idtxt = build_id.hex() if build_id else ""
    cmd = "devflash begin %d %d %s%s\n" % (len(image), crc, idtxt, " --force" if force else "")
    port.write(cmd.encode())
    print(read_text(port, 10).strip().splitlines()[-1:])

    sent = 0
    t0 = time.time()
    while sent < len(image):
        piece = image[sent:sent + chunk]
        port.write(("devflash chunk %d %s\n" % (sent, piece.hex())).encode())
        line = read_text(port, 10).strip()
        if "OK" not in line:
            print("chunk failed at %d: %s" % (sent, line))
            return False
        sent += len(piece)
        if sent % (chunk * 16) == 0 or sent == len(image):
            print("\r  %d/%d bytes (%.1f%%)" % (sent, len(image), 100.0 * sent / len(image)),
                  end="", flush=True)
    dt = max(1e-3, time.time() - t0)
    print("\n  transport: %.1f s, %.1f KB/s" % (dt, len(image) / 1024.0 / dt))
    port.write(b"devflash end\n")
    out = read_text(port, 30).strip()
    print(out.splitlines()[-1:] if out else "")
    return "OK image verified" in out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--image", required=True)
    ap.add_argument("--uart", action="store_true", help="hex text transport instead of frames")
    ap.add_argument("--chunk", type=int, default=0)
    ap.add_argument("--no-build-check", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    image = open(args.image, "rb").read()
    print("image %s: %d bytes, crc32 %08X" % (args.image, len(image), zlib.crc32(image) & 0xFFFFFFFF))

    port = serial.Serial(args.port, args.baud, timeout=0.2)
    time.sleep(0.2)
    port.reset_input_buffer()

    build_id = None if args.no_build_check else device_build_id(port)
    if build_id:
        print("device build id: %s" % build_id.hex())
    elif not args.no_build_check:
        print("warning: could not read the device build id; begin will not id-check")

    if args.uart:
        ok = upload_text(port, image, build_id, args.chunk or 256, args.force)
    else:
        if not enter_binary(port):
            print("could not enter binary mode; retry with --uart")
            port.close()
            return 2
        ok = upload_framed(port, image, build_id, args.chunk or 2048, args.force)
        leave_binary(port)

    port.close()
    if ok:
        print("done - run `reboot` (or power-cycle) to boot the new image")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
