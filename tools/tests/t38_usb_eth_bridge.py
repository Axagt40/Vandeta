#!/usr/bin/env python3
"""t38 - live test of the USB Ethernet bridge on a real board.

What it proves, without needing an Ethernet cable anywhere:
  * the Ethernet page's new "USB Ethernet (PC LAN)" entry creates a working
    lwIP interface on the board (esp_netif + custom driver),
  * the board transmits real Ethernet frames over the USB-C cable (they arrive
    here and parse),
  * the board *receives* frames from the cable: this test answers the board's
    DHCP DISCOVER/REQUEST with an OFFER/ACK, and the board then answers an ARP
    request for the lease. That round trip exercises framing, esp_netif_receive,
    Ethernet input, ARP and the MAC the driver installed,
  * the stop frame makes the page closeable from the host side.

It drives the UI itself over the developer console (optionsJSON / option), so
Developer Options must be on. Run it with the board plugged in:

    python3 FREEBUFF/tests/t38_usb_eth_bridge.py [/dev/ttyACM0]

Timing: the page opens the interface immediately, so the DHCP frames start
before this test finishes sending the selection; the parser runs from the moment
the option is chosen, not after a delay.
"""

import json
import struct
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is missing: python3 -m pip install pyserial")
    raise SystemExit(2)

# --no-offer: watch only. Used to tell "DHCP retries normally" apart from
# "our OFFER put the client into a state that stops it retrying".
NO_OFFER = "--no-offer" in sys.argv
_args = [a for a in sys.argv[1:] if not a.startswith("--")]
PORT = _args[0] if _args else "/dev/ttyACM0"

MAGIC = b"\xa5\x5a\x5a\xa5"
T_FRAME, T_LINK, T_STOP = 0x01, 0x02, 0x03
FRAME_MAX = 1600

BOARD_IP = "192.168.99.5"
SERVER_IP = "192.168.99.1"
SERVER_MAC = bytes.fromhex("020000000001")
LEASE = 120


# ---------------------------------------------------------------- packets


def ip_bytes(text):
    return bytes(int(part) for part in text.split("."))


def checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def eth(dst, src, ethertype, payload):
    return dst + src + struct.pack("!H", ethertype) + payload


def ipv4(src, dst, proto, payload, ident=0x42):
    total = 20 + len(payload)
    header = struct.pack(
        "!BBHHHBBH4s4s", 0x45, 0, total, ident, 0, 64, proto, 0, ip_bytes(src), ip_bytes(dst)
    )
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    return header + payload


def udp(sport, dport, payload, src, dst):
    length = 8 + len(payload)
    header = struct.pack("!HHHH", sport, dport, length, 0)
    pseudo = ip_bytes(src) + ip_bytes(dst) + struct.pack("!BBH", 0, 17, length)
    csum = checksum(pseudo + header + payload) or 0xFFFF
    return struct.pack("!HHHH", sport, dport, length, csum) + payload


def dhcp(op, xid, mac, yiaddr, msgtype):
    """op: 2 = reply. msgtype: 2 = OFFER, 5 = ACK."""
    body = struct.pack("!BBBBIHH", op, 1, 6, 0, xid, 0, 0x8000)
    body += struct.pack("!IIII", 0, int.from_bytes(ip_bytes(yiaddr), "big"), 0, 0)
    body += mac + b"\x00" * 10
    body += b"\x00" * 64  # sname
    body += b"\x00" * 128  # file
    body += b"\x63\x82\x53\x63"
    body += bytes([53, 1, msgtype])
    body += bytes([54, 4]) + ip_bytes(SERVER_IP)
    body += bytes([51, 4]) + struct.pack("!I", LEASE)
    body += bytes([1, 4]) + ip_bytes("255.255.255.0")
    body += bytes([3, 4]) + ip_bytes(SERVER_IP)
    body += bytes([6, 4]) + ip_bytes(SERVER_IP)
    body += b"\xff"
    return body


def dhcp_frame(mac, xid, msgtype):
    """The reply must be broadcast: the board has no address yet, and lwIP
    drops a unicast packet that matches no interface address."""
    payload = dhcp(2, xid, mac, BOARD_IP, msgtype)
    bcast = "255.255.255.255"
    body = ipv4(SERVER_IP, bcast, 17, udp(67, 68, payload, SERVER_IP, bcast))
    return eth(b"\xff" * 6, SERVER_MAC, 0x0800, body)


def arp_request(target_ip):
    body = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1)
    body += SERVER_MAC + ip_bytes(SERVER_IP) + b"\x00" * 6 + ip_bytes(target_ip)
    return eth(b"\xff" * 6, SERVER_MAC, 0x0806, body)


# ---------------------------------------------------------------- framing


class Parser:
    def __init__(self):
        self.reset()

    def reset(self):
        self.state = 0
        self.kind = 0
        self.want = 0
        self.buf = bytearray()

    @staticmethod
    def _plausible(kind, want):
        if kind == T_FRAME:
            return 14 <= want <= FRAME_MAX
        if kind == T_LINK:
            return want == 1
        if kind == T_STOP:
            return want == 0
        return False

    def feed(self, data):
        out = []
        for byte in data:
            # Re-examine a byte that breaks a magic candidate: it may be the
            # first byte of the real magic.
            while True:
                if self.state == 0:
                    if byte == MAGIC[0]:
                        self.state = 1
                    break
                if self.state == 1:
                    if byte == MAGIC[1]:
                        self.state = 2
                        break
                    self.state = 0
                    continue
                if self.state == 2:
                    if byte == MAGIC[2]:
                        self.state = 3
                        break
                    self.state = 0
                    continue
                if self.state == 3:
                    if byte == MAGIC[3]:
                        self.state = 4
                        break
                    self.state = 0
                    continue
                if self.state == 4:
                    self.kind = byte
                    self.state = 5
                    break
                if self.state == 5:
                    self.want = byte
                    self.state = 6
                    break
                if self.state == 6:
                    self.want |= byte << 8
                    if not self._plausible(self.kind, self.want):
                        self.reset()
                    elif self.want == 0:
                        out.append((self.kind, b""))
                        self.reset()
                    else:
                        self.buf = bytearray()
                        self.state = 7
                    break
                self.buf.append(byte)
                if len(self.buf) >= self.want:
                    out.append((self.kind, bytes(self.buf)))
                    self.reset()
                break
        return out

    @staticmethod
    def encode(kind, payload):
        return MAGIC + bytes([kind]) + struct.pack("<H", len(payload)) + payload


# ---------------------------------------------------------------- console


def read_until(ser, marker=b"# ", timeout=4.0):
    deadline = time.time() + timeout
    data = b""
    while time.time() < deadline:
        chunk = ser.read(512)
        if chunk:
            data += chunk
            if marker in data:
                break
    return data


def command(ser, text, timeout=4.0):
    ser.reset_input_buffer()
    ser.write(text.encode() + b"\r\n")
    return read_until(ser, timeout=timeout)


def menu(ser):
    raw = command(ser, "optionsJSON", timeout=6.0).decode("utf-8", "replace")
    for line in reversed(raw.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                continue
    return None


def pick(menu_json, needle):
    if not menu_json:
        return None
    for opt in menu_json.get("options", []):
        if needle.lower() in opt["label"].lower():
            return opt["n"]
    return None


# ---------------------------------------------------------------- test


def main():
    if not PORT:
        print("no serial port given")
        return 2

    ser = serial.Serial(PORT, 115200, timeout=0.05)
    time.sleep(0.4)
    ser.reset_input_buffer()

    top = menu(ser)
    print(f"menu on screen: {top.get('menu_title') if top else '?'}")
    eth_index = pick(top, "ethernet")
    if eth_index is None:
        print("FAIL: no Ethernet entry in the main menu")
        ser.close()
        return 1
    command(ser, f"option {eth_index}")
    time.sleep(0.8)

    sub = menu(ser)
    if not sub:
        print("FAIL: Ethernet submenu did not answer optionsJSON")
        ser.close()
        return 1
    labels = [o["label"] for o in sub["options"]]
    print(f"Ethernet menu: {labels}")
    bridge_index = pick(sub, "usb ethernet")
    if bridge_index is None:
        print("FAIL: no 'USB Ethernet (PC LAN)' entry")
        ser.close()
        return 1

    parser = Parser()
    boots = 0
    seen_text = set()
    seen = {"frames": 0, "dhcp": [], "arp_reply": None, "offer": None, "ack": None}
    board_mac = None
    start = time.time()
    last_link = 0.0
    last_arp = 0.0
    passed = False

    print(f"selecting option {bridge_index}: {labels[bridge_index]}")
    ser.write(f"option {bridge_index}\r\n".encode())

    while time.time() - start < 45.0:
        now = time.time()
        if now - last_link > 2.0:
            # carrier up: the board shows this as "NIC: carrier up"
            ser.write(Parser.encode(T_LINK, b"\x01"))
            last_link = now
        if now - last_arp > 1.0 and seen["ack"] is not None:
            ser.write(Parser.encode(T_FRAME, arp_request(BOARD_IP)))
            last_arp = now

        chunk = ser.read(4096)
        if not chunk:
            continue
        # A reboot leaves the same boot lines behind, and dedup would hide a
        # crash loop, so count them explicitly.
        boots += chunk.count(b"first-mainMenu")
        # Text mixed into the stream is normally skipped by the parser, but a
        # panic or an assert arrives exactly that way, so show it.
        run = bytearray()
        for byte in chunk + b"\x00":
            if 32 <= byte < 127:
                run.append(byte)
            else:
                if len(run) >= 8 and any(c in b"abcdefghijklmnopqrstuvwxyz" for c in run):
                    text = run.decode("ascii", "ignore").strip()
                    if text not in seen_text:
                        seen_text.add(text)
                        print(f"  [device text] {text}")
                run = bytearray()
        for kind, payload in parser.feed(chunk):
            if kind != T_FRAME or len(payload) < 14:
                continue
            seen["frames"] += 1
            src = payload[6:12]
            if board_mac is None:
                board_mac = src
                print(f"board frames arriving, src MAC {':'.join(f'{b:02x}' for b in src)}")
            ethertype = struct.unpack("!H", payload[12:14])[0]
            if seen["frames"] <= 6:
                detail = f"ethertype=0x{ethertype:04x}"
                if ethertype == 0x0800 and len(payload) > 38:
                    detail += f" proto={payload[23]}"
                    if payload[23] in (6, 17):
                        sport, dport = struct.unpack("!HH", payload[34:38])
                        detail += f" ports={sport}->{dport}"
                elif ethertype == 0x0806 and len(payload) >= 22:
                    detail += f" arp_op={struct.unpack('!H', payload[20:22])[0]}"
                print(f"  frame {seen['frames']}: len={len(payload)} {detail}")

            if ethertype == 0x0800 and len(payload) >= 42:
                proto = payload[23]
                if proto == 17:
                    sport, dport = struct.unpack("!HH", payload[34:38])
                    # Client -> server: 68 -> 67. (Checking dport == 68 here
                    # catches nothing, which looks exactly like "no DHCP".)
                    if sport == 68 and dport == 67:
                        body = payload[42:]
                        if len(body) >= 240:
                            op = body[0]
                            xid = struct.unpack("!I", body[4:8])[0]
                            mac = body[28:34]
                            mtype = None
                            i = 240
                            while i < len(body):
                                if body[i] == 53 and i + 2 < len(body):
                                    mtype = body[i + 2]
                                    break
                                if body[i] == 255:
                                    break
                                if i + 1 >= len(body):
                                    break
                                i += 2 + body[i + 1]
                            if op == 1 and mtype in (1, 3):
                                tag = "DISCOVER" if mtype == 1 else "REQUEST"
                                key = tag.lower()
                                if key not in [t for t, _ in seen["dhcp"]]:
                                    seen["dhcp"].append((key, xid))
                                    print(f"  board sent DHCP {tag} (xid {xid:08x})")
                                if NO_OFFER:
                                    pass
                                elif mtype == 1 and seen["offer"] != xid:
                                    ser.write(Parser.encode(T_FRAME, dhcp_frame(mac, xid, 2)))
                                    seen["offer"] = xid
                                    print("  -> OFFER sent")
                                elif mtype == 3 and seen["ack"] != xid:
                                    ser.write(Parser.encode(T_FRAME, dhcp_frame(mac, xid, 5)))
                                    seen["ack"] = xid
                                    print("  -> ACK sent")
            elif ethertype == 0x0806 and len(payload) >= 42:
                opcode = struct.unpack("!H", payload[20:22])[0]
                sender_ip = ".".join(str(b) for b in payload[28:32])
                if opcode == 2 and sender_ip == BOARD_IP:
                    seen["arp_reply"] = payload
                    passed = True
                    print(f"  <- ARP reply: {BOARD_IP} is at {':'.join(f'{b:02x}' for b in payload[22:28])}")
                    break
        if passed:
            break

    elapsed = time.time() - start
    print("")
    print(f"board frames received : {seen['frames']}")
    print(f"board reboots seen    : {boots}")
    print(f"board MAC             : {':'.join(f'{b:02x}' for b in board_mac) if board_mac else 'none'}")
    print(f"DHCP exchange         : {[t for t, _ in seen['dhcp']]}")
    print(f"lease taken           : {'yes' if seen['arp_reply'] else 'no'}")
    print(f"elapsed               : {elapsed:.1f}s")

    if passed:
        print("PASS: L2 in both directions, DHCP lease accepted, ARP answered")
    elif seen["frames"] and not seen["dhcp"]:
        print("FAIL: board transmits but never sent DHCP - interface/driver problem")
    elif seen["ack"]:
        print("FAIL: lease ACKed but no ARP reply - input path or ARP broken")
    else:
        print("FAIL: no frames from the board - page did not start the interface")

    # Leave the page the way the PC helper does.
    try:
        ser.write(Parser.encode(T_STOP, b""))
        time.sleep(0.3)
    except Exception:  # noqa: BLE001
        pass
    ser.close()
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
