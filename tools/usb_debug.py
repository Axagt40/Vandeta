#!/usr/bin/env python3
"""Host client for the Bruce "Developer Options" mode (T-Embed CC1101 over USB-CDC).

Two things it can talk:

  text     the normal line based Bruce CLI. Everything the device prints is
           passed through.
  binary   the framed protocol added by the Developer Options mode. Faster and
           unambiguous for file transfer and scripted command runs.

Frame layout (little endian):

    A5 5A  type  len_lo  len_hi  payload...  crc_lo  crc_hi

    crc = CRC-16/CCITT-FALSE (init 0xFFFF, poly 0x1021) over
          type + len_lo + len_hi + payload

    request            response                meaning
    0x01 PING          0x81 MSG                liveness / free text
    0x02 EXEC          0x82 EXEC_OK            payload = one CLI command line
    0x04 PUT_BEGIN     0x85 PUT_ACK           payload = path \\0 size(4)
    0x05 PUT_DATA      0x85 PUT_ACK           payload = offset(4) + bytes
    0x06 PUT_END       0x86 PUT_DONE          closes the upload
    0x07 GET_BEGIN     0x87 GET_INFO           payload = path (stat only: size)
    0x08 GET_DATA      0x88 GET_DATA           payload = path \0 offset(4) len(2)
                       0x89 GET_DONE           offset >= size (end of file)
    0x09 SET_MODE      0x8A MODE             payload[0] 1 = binary, 0 = text
    0x0A EXEC_BATCH    0x82 EXEC_OK          payload = newline separated commands
    0x0B INFO          0x8B INFO             key=value lines

IMPORTANT: opening the serial port RESETS the board, and the firmware always
boots in TEXT mode (a deliberate safety property, so a host that got the
protocol wrong can never lock itself out). Every structured op therefore does:

    wait for boot  ->  send the TEXT command `proto binary`  ->  do the work
                   ->  send SET_MODE(0)  ->  back to text

The ENTRY into the framed protocol is that text command on purpose: text mode
never inspects incoming bytes (it hands the port to the stock CLI, which raw
commands like `storage write` depend on), so a frame sent while the device is
still in text mode would be parsed as a command line. Once the device has
printed "binary protocol ON", it only speaks frames.

Usage:
  python3 tools/usb_debug.py [port] monitor
  python3 tools/usb_debug.py [port] info
  python3 tools/usb_debug.py [port] exec "ls /BruceJS"
  python3 tools/usb_debug.py [port] put ./myscript.js /BruceJS/myscript.js
  python3 tools/usb_debug.py [port] get /BruceJS/myscript.js ./back.js
  python3 tools/usb_debug.py [port] batch ./cmds.txt

Requires: Developer Options enabled on the device (Config > System Config >
Developer Options, or `usbdebug on` over the text console).
"""

import os
import struct
import sys
import time

import serial

MAGIC = b"\xa5\x5a"

FR_PING = 0x01
FR_EXEC = 0x02
FR_PUT_BEGIN = 0x04
FR_PUT_DATA = 0x05
FR_PUT_END = 0x06
FR_GET_BEGIN = 0x07
FR_GET_DATA = 0x08
FR_SET_MODE = 0x09
FR_EXEC_BATCH = 0x0A
FR_INFO = 0x0B

R_MSG = 0x81
R_EXEC_OK = 0x82
R_PUT_ACK = 0x85
R_PUT_DONE = 0x86
R_GET_INFO = 0x87
R_GET_DATA = 0x88
R_GET_DONE = 0x89
R_MODE = 0x8A
R_INFO = 0x8B

CHUNK = 2048
MAX_PAYLOAD = 2100


# ---------------------------------------------------------------------------
# CRC-16/CCITT-FALSE
# ---------------------------------------------------------------------------
def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_frame(ftype: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too big: %d" % len(payload))
    head = bytes([ftype, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    crc = crc16(head + payload)
    return MAGIC + head + payload + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


# ---------------------------------------------------------------------------
# Port helpers
# ---------------------------------------------------------------------------
def open_port(port: str) -> serial.Serial:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.05
    s.dtr = False
    s.rts = False
    s.open()
    s.dtr = False
    s.rts = False
    return s


class Reader:
    """Byte stream -> (text chunks, decoded frames) with resync on the magic."""

    def __init__(self, port: serial.Serial):
        self.port = port
        self.buf = bytearray()
        self.pending = []  # frames already decoded but not yet consumed

    def pump(self, timeout=0.2, until_frame=False):
        """Return (text_bytes, [frames]). Frames are (type, payload).

        Frames already decoded by an earlier call are returned first, so a
        caller that only wanted the first frame never loses the rest.

        until_frame=True returns as soon as one frame is decoded instead of
        waiting out the whole quiet period. Without it every request/response
        round trip pays the full timeout, which dominates transfer time.
        """
        deadline = time.time() + timeout
        text = bytearray()
        frames = self.pending
        self.pending = []
        if until_frame and frames:
            return bytes(text), frames
        while time.time() < deadline:
            n = self.port.in_waiting
            if n:
                self.buf.extend(self.port.read(n))
                deadline = time.time() + timeout
            else:
                time.sleep(0.01)

            while True:
                start = self.buf.find(MAGIC)
                if start < 0:
                    # keep the last byte in case it is the first magic byte
                    if len(self.buf) > 1:
                        text.extend(self.buf[:-1])
                        del self.buf[:-1]
                    break
                if start > 0:
                    text.extend(self.buf[:start])
                    del self.buf[:start]
                if len(self.buf) < 5:
                    break
                ptype = self.buf[2]
                plen = self.buf[3] | (self.buf[4] << 8)
                if plen > MAX_PAYLOAD:
                    text.extend(self.buf[:2])
                    del self.buf[:2]
                    continue
                total = 5 + plen + 2
                if len(self.buf) < total:
                    break
                body = bytes(self.buf[2:5 + plen])
                crc = self.buf[5 + plen] | (self.buf[6 + plen] << 8)
                if crc != crc16(body):
                    # false magic inside plain text: drop one byte and resync
                    text.extend(self.buf[:1])
                    del self.buf[:1]
                    continue
                frames.append((ptype, bytes(self.buf[5:5 + plen])))
                del self.buf[:total]
                if until_frame:
                    return bytes(text), frames
        return bytes(text), frames


def wait_for_boot(reader: Reader, seconds=20, verbose=False):
    """Wait until the firmware prints its boot banner, then settle."""
    deadline = time.time() + seconds
    seen = False
    while time.time() < deadline:
        text, frames = reader.pump(0.5)
        if verbose and text:
            sys.stdout.buffer.write(text)
            sys.stdout.buffer.flush()
        if b"[USBDBG]" in text or b"setup-end" in text:
            seen = True
            break
    time.sleep(1.0)
    reader.pump(0.5)
    return seen


def send(port, ftype, payload=b""):
    port.write(build_frame(ftype, payload))
    port.flush()


def expect(reader, wanted, timeout=10.0):
    """Collect frames until one of `wanted` arrives. Returns (type, payload).

    Frames that do not match are pushed back for the next call instead of
    being dropped.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        text, frames = reader.pump(0.3, until_frame=True)
        if text:
            sys.stdout.buffer.write(text)
            sys.stdout.buffer.flush()
        for index, (ftype, payload) in enumerate(frames):
            if ftype in wanted:
                reader.pending = list(frames[index + 1:]) + reader.pending
                return ftype, payload
            print("[frame 0x%02X] %s" % (ftype, payload.decode("utf8", "replace")))
    return None, None


def enter_binary(port, reader, timeout=8.0):
    """Switch the device to the framed protocol using the TEXT entry command.

    If the text handshake is not confirmed, the device may simply still be in
    binary mode from an earlier session: opening the port does not always reset
    the chip (that depends on how the host toggles DTR/RTS), in which case the
    text command is parsed as a frame and ignored. A PING tells the two states
    apart - binary mode answers with a frame, text mode answers with a command
    error - so a stale binary session is re-used instead of failing.
    """
    port.write(b"proto binary\n")
    port.flush()
    deadline = time.time() + timeout
    while time.time() < deadline:
        text, _ = reader.pump(0.3)
        if text:
            if b"binary protocol ON" in text:
                return True
        time.sleep(0.05)

    # Fallback: already in binary mode?
    send(port, FR_PING)
    ftype, payload = expect(reader, {R_MSG}, timeout=2.0)
    if ftype == R_MSG:
        print("[usb_debug] device was already in binary mode", file=sys.stderr)
        return True
    return False


def set_mode(port, reader, binary: bool):
    """Leave binary mode (and confirm); entering uses the text command."""
    if binary:
        return enter_binary(port, reader)
    send(port, FR_SET_MODE, bytes([0]))
    ftype, payload = expect(reader, {R_MODE}, timeout=5)
    if ftype is None:
        print("warning: no MODE ack")
        return False
    print(payload.decode("utf8", "replace"))
    return True


# ---------------------------------------------------------------------------
# Operations
# ---------------------------------------------------------------------------
def op_info(port, reader):
    send(port, FR_INFO)
    ftype, payload = expect(reader, {R_INFO})
    if payload:
        print(payload.decode("utf8", "replace"))


def op_ping(port, reader):
    send(port, FR_PING)
    expect(reader, {R_MSG})


def op_exec(port, reader, command: str, settle=5.0, cap=120.0):
    """Send one command line.

    The ACK proves the device accepted the frame, not that the command has
    finished printing - `js <script>` in particular keeps writing long after
    it. Returning on the ACK silently threw that output away, so keep reading
    until the device has been quiet for `settle` seconds, with a `cap` on the
    whole operation so a wedged command cannot hang the host forever.
    """
    send(port, FR_EXEC, command.encode())
    expect(reader, {R_EXEC_OK}, timeout=30)

    deadline = time.time() + cap
    quiet = 0.0
    while time.time() < deadline:
        text, frames = reader.pump(0.5)
        if text:
            sys.stdout.buffer.write(text)
            sys.stdout.buffer.flush()
            quiet = 0.0
        for ftype, payload in frames:
            print("[frame 0x%02X] %s" % (ftype, payload.decode("utf8", "replace")))
        if not text and not frames:
            quiet += 0.5
            if quiet >= settle:
                return
    print("WARNING: still receiving output after %.0fs; stopping" % cap, file=sys.stderr)


def op_batch(port, reader, path: str):
    with open(path, "r") as fh:
        body = fh.read()
    send(port, FR_EXEC_BATCH, body.encode())
    expect(reader, {R_EXEC_OK})

    # Wait for the device's own end-of-batch marker instead of guessing a
    # deadline from the line count. A batch full of radio attacks takes far
    # longer than a batch of `ls` calls, and an under-estimated deadline used
    # to make this client stop reading and close the port while the device was
    # still working - which looked exactly like the batch being truncated.
    # The floor and the per-line budget are generous; the marker ends it early.
    lines = max(1, len(body.splitlines()))
    deadline = time.time() + max(30.0, 12.0 * lines)
    quiet = 0.0  # seconds since the device last said anything
    seen = bytearray()
    while time.time() < deadline:
        text, _frames = reader.pump(0.5)
        if text:
            sys.stdout.buffer.write(text)
            sys.stdout.buffer.flush()
            seen.extend(text)
            quiet = 0.0
            if b"batch done" in seen:
                # The device's queue holds 32 commands; anything past that is
                # dropped when the batch is submitted. Say so instead of
                # reporting a clean run over a truncated batch.
                if b"Queue full" in seen:
                    print(
                        "WARNING: the device dropped commands (queue full, 32 max); "
                        "split this batch and re-run it",
                        file=sys.stderr,
                    )
                return  # the device confirms every queued command has run
        else:
            quiet += 0.5
            # A drained batch goes silent. The longest single command here is a
            # radio attack (a few seconds), so a long quiet gap means it is over.
            if quiet >= 10.0:
                break
    print(
        "WARNING: the device never reported 'batch done' - the output above may be partial",
        file=sys.stderr,
    )


def op_put(port, reader, local: str, remote: str):
    size = os.path.getsize(local)
    payload = remote.encode() + b"\x00" + struct.pack("<I", size)
    send(port, FR_PUT_BEGIN, payload)
    ftype, resp = expect(reader, {R_PUT_ACK, R_MSG})
    print(resp.decode("utf8", "replace") if resp else "no response")
    if ftype != R_PUT_ACK or not resp.startswith(b"READY"):
        return False

    sent = 0
    with open(local, "rb") as fh:
        while True:
            chunk = fh.read(CHUNK)
            if not chunk:
                break
            send(port, FR_PUT_DATA, struct.pack("<I", sent) + chunk)
            ftype, resp = expect(reader, {R_PUT_ACK, R_MSG}, timeout=20)
            if ftype != R_PUT_ACK or not resp.startswith(b"CHUNK"):
                print("upload failed at offset %d: %s" % (sent, resp))
                return False
            sent += len(chunk)
            pct = 100.0 * sent / size if size else 100.0
            print("\r  uploaded %d/%d bytes (%.1f%%)" % (sent, size, pct), end="")
            sys.stdout.flush()

    send(port, FR_PUT_END)
    ftype, resp = expect(reader, {R_PUT_DONE})
    print("\n" + (resp.decode("utf8", "replace") if resp else "no response"))
    return ftype == R_PUT_DONE


def op_get(port, reader, remote: str, local: str):
    """Pull the file in chunks. The device answers FR_GET_DATA per chunk and
    FR_GET_DONE once the requested offset is past the end."""
    send(port, FR_GET_BEGIN, remote.encode())
    ftype, resp = expect(reader, {R_GET_INFO, R_MSG})
    print(resp.decode("utf8", "replace") if resp else "no response")
    if ftype != R_GET_INFO or not resp.startswith(b"OK"):
        return False

    total = int(resp.split()[-1])
    received = bytearray()
    while True:
        payload = remote.encode() + b"\x00" + struct.pack("<IH", len(received), CHUNK)
        send(port, FR_GET_DATA, payload)
        ftype, resp = expect(reader, {R_GET_DATA, R_GET_DONE, R_MSG}, timeout=20)
        if ftype is None:
            print("\ntimeout at offset %d" % len(received))
            return False
        if ftype == R_GET_DONE:
            print("\n" + resp.decode("utf8", "replace"))
            with open(local, "wb") as fh:
                fh.write(received)
            if len(received) != total:
                print("SIZE MISMATCH: got %d want %d" % (len(received), total))
                return False
            return True
        if ftype == R_MSG:
            print("\nerror at offset %d: %s" % (len(received), resp))
            return False
        offset = struct.unpack("<I", resp[:4])[0]
        if offset != len(received):
            print("\noffset mismatch: want %d got %d" % (len(received), offset))
            return False
        received.extend(resp[4:])
        pct = 100.0 * len(received) / total if total else 100.0
        print("\r  downloaded %d/%d bytes (%.1f%%)" % (len(received), total, pct), end="")
        sys.stdout.flush()


def op_monitor(port, reader, seconds=0):
    end = time.time() + seconds if seconds else None
    while True:
        if end and time.time() > end:
            return
        text, frames = reader.pump(0.3)
        if text:
            sys.stdout.buffer.write(text)
            sys.stdout.buffer.flush()
        for ftype, payload in frames:
            print(
                "\n[frame 0x%02X len=%d] %s"
                % (ftype, len(payload), payload.decode("utf8", "replace"))
            )


# ---------------------------------------------------------------------------
def main():
    argv = sys.argv[1:]
    port_name = "/dev/ttyACM0"
    if argv and not argv[0].startswith("-") and argv[0].startswith("/"):
        if "/" in argv[0] or argv[0].startswith("/dev"):
            port_name = argv.pop(0)
    if not argv:
        print(__doc__)
        return 2

    op = argv.pop(0)
    structured = op in ("info", "ping", "exec", "put", "get", "batch")

    port = open_port(port_name)
    reader = Reader(port)
    print("waiting for the device to boot (opening the port resets it)...", file=sys.stderr)
    wait_for_boot(reader)

    ok = True
    try:
        if structured:
            if not set_mode(port, reader, True):
                print("ERROR: could not switch the device to the binary protocol", file=sys.stderr)
                return 1

        if op == "monitor":
            op_monitor(port, reader)
        elif op == "info":
            op_info(port, reader)
        elif op == "ping":
            op_ping(port, reader)
        elif op == "exec":
            op_exec(port, reader, " ".join(argv))
        elif op == "batch":
            op_batch(port, reader, argv[0])
        elif op == "put":
            ok = op_put(port, reader, argv[0], argv[1])
        elif op == "get":
            ok = op_get(port, reader, argv[0], argv[1])
        else:
            print("unknown op: %s" % op, file=sys.stderr)
            ok = False
    except KeyboardInterrupt:
        pass
    finally:
        if structured:
            try:
                set_mode(port, reader, False)
            except Exception:  # noqa: BLE001 - best effort on the way out
                pass
        port.close()

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
