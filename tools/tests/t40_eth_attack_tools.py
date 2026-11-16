#!/usr/bin/env python3
"""t40 - the three Ethernet attack tools, driven and watched over the USB bridge.

The board cannot reach a LAN by itself, so this test plays one: a veth pair, the
bridge carried in-process (the helper would hold the serial port, and the test
needs that port to press keys), and a fake DHCP server / ARP host on the far
end. Nothing here touches a real network - which is the point, because the
tools under test are an ARP scan, a DHCP starvation and a MAC flood.

What each check proves:

  DHCP Starvation  frames really leave the board through the bridge, and each
                   one is a *valid* DISCOVER (magic cookie, option 53, chaddr ==
                   source MAC, IP checksum) with a fresh unicast source MAC.
                   A malformed DISCOVER is what makes the tool useless against a
                   real server, so validity is checked, not just the count.
  MAC Flooding     the flood reaches the wire with a distinct source MAC per
                   frame (that is what fills a switch's CAM table).
  Scan Hosts       the scanner's ARP requests walk the /24, and a host that
                   answers is reached through the bridge.

It also proves the key frame: the text console is suspended while bridging, so
each tool is started and stopped from here with remote keypresses instead of the
physical buttons.

    sudo python3 FREEBUFF/tests/t40_eth_attack_tools.py [/dev/ttyACM0]

Root is needed (veth + AF_PACKET). Developer Options must be on for the console
navigation before the bridge takes the port.
"""

import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("pyserial is missing: python3 -m pip install pyserial")
    raise SystemExit(2)

_args = [a for a in sys.argv[1:] if not a.startswith("--")]
PORT = _args[0] if _args else "/dev/ttyACM0"

MAGIC = b"\xa5\x5a\x5a\xa5"
T_FRAME, T_LINK, T_STOP, T_KEY = 0x01, 0x02, 0x03, 0x04
FRAME_MAX = 1600

KEY_CODES = {"back": 0, "ok": 1, "up": 2, "down": 3, "left": 4, "right": 5}

BOARD_IP = "192.168.99.5"
SERVER_IP = "192.168.99.1"
SERVER_MAC = bytes.fromhex("020000000001")
HOST_IP = "192.168.99.7"
HOST_MAC = bytes.fromhex("020000000007")
LEASE = 120

VD_BRIDGE = "vta0"  # the board is bridged onto this end
VD_SERVER = "vta1"  # this test answers DHCP/ARP on its peer

ETH_P_ALL = 0x0003
PACKET_OUTGOING = 4
SOL_PACKET, PACKET_ADD_MEMBERSHIP, PACKET_MR_PROMISC = 263, 1, 1


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


def ipv4(src, dst, proto, payload):
    total = 20 + len(payload)
    header = struct.pack(
        "!BBHHHBBH4s4s", 0x45, 0, total, 0x42, 0, 64, proto, 0, ip_bytes(src), ip_bytes(dst)
    )
    return header[:10] + struct.pack("!H", checksum(header)) + header[12:] + payload


def udp(sport, dport, payload, src, dst):
    length = 8 + len(payload)
    header = struct.pack("!HHHH", sport, dport, length, 0)
    pseudo = ip_bytes(src) + ip_bytes(dst) + struct.pack("!BBH", 0, 17, length)
    csum = checksum(pseudo + header + payload) or 0xFFFF
    return struct.pack("!HHHH", sport, dport, length, csum) + payload


def dhcp_reply(mac, xid, msgtype, yiaddr):
    body = struct.pack("!BBBBIHH", 2, 1, 6, 0, xid, 0, 0x8000)
    body += struct.pack("!IIII", 0, int.from_bytes(ip_bytes(yiaddr), "big"), 0, 0)
    body += mac + b"\x00" * 10 + b"\x00" * 64 + b"\x00" * 128
    body += b"\x63\x82\x53\x63"
    body += bytes([53, 1, msgtype]) + bytes([54, 4]) + ip_bytes(SERVER_IP)
    body += bytes([51, 4]) + struct.pack("!I", LEASE) + bytes([1, 4]) + ip_bytes("255.255.255.0")
    body += bytes([3, 4]) + ip_bytes(SERVER_IP) + b"\xff"
    bcast = "255.255.255.255"
    return eth(b"\xff" * 6, SERVER_MAC, 0x0800, ipv4(SERVER_IP, bcast, 17, udp(67, 68, body, SERVER_IP, bcast)))


def arp_reply(target_ip, target_mac):
    body = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2)
    body += target_mac + ip_bytes(target_ip) + SERVER_MAC + ip_bytes(SERVER_IP)
    # ARP is always broadcast here: the board's MAC is known to this test anyway,
    # but a broadcast reply is what a host that has not learnt it would send.
    return eth(b"\xff" * 6, target_mac, 0x0806, body)


# ---------------------------------------------------------------- framing


class Parser:
    """Same four-magic framing as UsbEthBridge.cpp, one byte at a time."""

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
        if kind == T_KEY:
            return want == 1
        return False

    def feed(self, data):
        out = []
        for byte in data:
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
    def encode(kind, payload=b""):
        return MAGIC + bytes([kind]) + struct.pack("<H", len(payload)) + payload


# ---------------------------------------------------------------- bridge


class Bridge:
    """The host half of the bridge, in-process so this test owns the port."""

    def __init__(self, iface, port):
        self.iface = iface
        self.port_name = port
        self.stop = threading.Event()
        self.serial = None
        self.sock = None
        self.counts = {"to_wire": 0, "keys": 0}
        self.lock = threading.Lock()

    def open(self):
        self.serial = serial.Serial(self.port_name, 115200, timeout=0.2)
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        self.sock.bind((self.iface, 0))
        mreq = struct.pack("IHH8s", socket.if_nametoindex(self.iface), PACKET_MR_PROMISC, 0, b"\x00" * 8)
        self.sock.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, mreq)
        self.sock.settimeout(0.2)

    def write(self, kind, payload=b""):
        with self.lock:
            self.serial.write(Parser.encode(kind, payload))
            self.serial.flush()

    def key(self, name, gap=0.5):
        code = KEY_CODES[name]
        self.write(T_KEY, bytes([code]))
        self.counts["keys"] += 1
        time.sleep(gap)

    def rx_loop(self):
        parser = Parser()
        while not self.stop.is_set():
            try:
                chunk = self.serial.read(4096)
            except Exception as exc:  # noqa: BLE001
                print(f"  serial read failed: {exc}")
                self.stop.set()
                return
            for kind, payload in parser.feed(chunk):
                if kind == T_FRAME and len(payload) >= 14:
                    try:
                        self.sock.send(payload)
                        self.counts["to_wire"] += 1
                    except OSError:
                        pass
            if not chunk:
                time.sleep(0.005)

    def wire_loop(self):
        """Frames from the fake LAN back to the board (the helper's other half)."""
        while not self.stop.is_set():
            try:
                frame, addr = self.sock.recvfrom(FRAME_MAX)
            except socket.timeout:
                continue
            except OSError:
                return
            if addr[2] == PACKET_OUTGOING or len(frame) < 14:
                continue
            try:
                self.write(T_FRAME, frame)
            except Exception:  # noqa: BLE001
                return

    def link_loop(self):
        while not self.stop.wait(2.0):
            try:
                self.write(T_LINK, b"\x01")
            except Exception:  # noqa: BLE001
                return

    def close(self):
        # Tell the board the page can close, exactly like eth_bridge.py does.
        try:
            self.write(T_STOP)
        except Exception:  # noqa: BLE001
            pass
        self.stop.set()
        time.sleep(0.4)
        try:
            self.serial.close()
        except Exception:  # noqa: BLE001
            pass
        try:
            self.sock.close()
        except Exception:  # noqa: BLE001
            pass


# ---------------------------------------------------------------- fake LAN


class FakeLan:
    """DHCP server + two ARP hosts on the far side of the bridge."""

    def __init__(self, iface):
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        self.sock.bind((iface, 0))
        mreq = struct.pack("IHH8s", socket.if_nametoindex(iface), PACKET_MR_PROMISC, 0, b"\x00" * 8)
        self.sock.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, mreq)
        self.sock.settimeout(0.05)
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.frames = []  # (monotonic time, frame) the board sent
        self.board_mac = None
        self.acked = False
        self.offered = False

    def mark(self):
        with self.lock:
            return len(self.frames)

    def since(self, mark):
        with self.lock:
            return [f for _, f in self.frames[mark:]]

    def run(self):
        while not self.stop.is_set():
            try:
                frame, addr = self.sock.recvfrom(FRAME_MAX)
            except socket.timeout:
                continue
            except OSError:
                return
            if addr[2] == PACKET_OUTGOING or len(frame) < 14:
                continue
            with self.lock:
                self.frames.append((time.monotonic(), frame))
            try:
                self.respond(frame)
            except Exception as exc:  # noqa: BLE001
                print(f"  responder error: {exc}")

    def send(self, frame):
        try:
            self.sock.send(frame)
        except OSError:
            pass

    def respond(self, frame):
        ethertype = struct.unpack("!H", frame[12:14])[0]
        if ethertype == 0x0800 and len(frame) >= 42 and frame[23] == 17:
            sport, dport = struct.unpack("!HH", frame[34:38])
            if (sport, dport) != (68, 67):
                return
            body = frame[42:]
            if len(body) < 240 or body[0] != 1 or body[236:240] != b"\x63\x82\x53\x63":
                return
            mac = body[28:34]
            xid = struct.unpack("!I", body[4:8])[0]
            msgtype = option(body, 53)  # one byte, e.g. b"\x01" for DISCOVER
            # Only the board's own lease is served; the starvation tool's random
            # MACs are exactly what a real server must run out of addresses for.
            if self.board_mac is None:
                self.board_mac = mac
            if mac != self.board_mac:
                return
            if msgtype == b"\x01" and not self.offered:
                self.send(dhcp_reply(mac, xid, 2, BOARD_IP))
                self.offered = True
            elif msgtype == b"\x03" and not self.acked:
                self.send(dhcp_reply(mac, xid, 5, BOARD_IP))
                self.acked = True
        elif ethertype == 0x0806 and len(frame) >= 42:
            if struct.unpack("!H", frame[20:22])[0] != 1:
                return
            target = frame[38:42]
            if target == ip_bytes(SERVER_IP):
                self.send(arp_reply(SERVER_IP, SERVER_MAC))
            elif target == ip_bytes(HOST_IP):
                self.send(arp_reply(HOST_IP, HOST_MAC))

    def close(self):
        self.stop.set()
        self.sock.close()


def option(body, code):
    """First value of a DHCP option, or None."""
    i = 240
    while i < len(body):
        if body[i] == 255:
            return None
        if body[i] == 0:
            i += 1
            continue
        if i + 1 >= len(body):
            return None
        length = body[i + 1]
        if body[i] == code:
            return body[i + 2 : i + 2 + length]
        i += 2 + length
    return None


# ---------------------------------------------------------------- wire checks


def ethertype(frame):
    return struct.unpack("!H", frame[12:14])[0]


def src_mac(frame):
    return frame[6:12]


def is_unicast(mac):
    return (mac[0] & 0x01) == 0


def ip_checksum_ok(frame):
    return checksum(frame[14:34]) == 0


def dhcp_discover_problems(frame):
    """Every reason this frame would be rejected by a real DHCP server."""
    problems = []
    if len(frame) < 286:
        problems.append(f"short ({len(frame)}B)")
        return problems
    if ethertype(frame) != 0x0800:
        problems.append("not IPv4")
        return problems
    if frame[23] != 17:
        problems.append("not UDP")
    if struct.unpack("!H", frame[34:36])[0] != 68 or struct.unpack("!H", frame[36:38])[0] != 67:
        problems.append("not 68->67")
    if frame[26:30] != b"\x00\x00\x00\x00" or frame[30:34] != b"\xff\xff\xff\xff":
        problems.append("wrong IP pair")
    if not ip_checksum_ok(frame):
        problems.append("bad IP checksum")
    body = frame[42:]
    if body[0] != 1:
        problems.append("not BOOTREQUEST")
    if body[236:240] != b"\x63\x82\x53\x63":
        problems.append("no DHCP magic cookie")
    if option(body, 53) != b"\x01":
        problems.append("no option 53 = DISCOVER")
    if body[28:34] != src_mac(frame):
        problems.append("chaddr != source MAC")
    if not is_unicast(src_mac(frame)):
        problems.append("multicast source MAC")
    return problems


# ---------------------------------------------------------------- console


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def console(cmd, timeout=6.0):
    ser = serial.Serial(PORT, 115200, timeout=0.05)
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(cmd.encode() + b"\r\n")
    data = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(512)
        if chunk:
            data += chunk
            if b"# " in data:
                break
    ser.close()
    return data.decode("utf-8", "replace")


def menu(title_cmd="optionsJSON"):
    for line in reversed(console(title_cmd).splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                pass
    return None


def pick(menu_json, needle):
    if not menu_json:
        return None
    for opt in menu_json.get("options", []):
        if needle.lower() in opt["label"].lower():
            return opt["n"]
    return None


def send_stop_frame():
    ser = serial.Serial(PORT, 115200, timeout=0.2)
    time.sleep(0.2)
    ser.write(Parser.encode(T_STOP))
    ser.flush()
    time.sleep(0.1)
    ser.close()


def ensure_console():
    if menu() is not None:
        return True
    print("board is on the bridge page; sending the stop frame to close it")
    send_stop_frame()
    for _ in range(8):
        time.sleep(0.6)
        if menu() is not None:
            return True
    return False


# ---------------------------------------------------------------- test


def veth_up():
    sh(f"ip link del {VD_BRIDGE} 2>/dev/null")
    out = sh(f"ip link add {VD_BRIDGE} type veth peer name {VD_SERVER}")
    if out.returncode != 0:
        print(f"cannot create veth: {out.stderr.strip()}")
        return False
    sh(f"ip link set {VD_BRIDGE} up")
    sh(f"ip link set {VD_SERVER} up")
    return True


def veth_down():
    sh(f"ip link del {VD_BRIDGE} 2>/dev/null")


def is_discover(frame):
    return (
        ethertype(frame) == 0x0800
        and len(frame) >= 286
        and frame[23] == 17
        and struct.unpack("!H", frame[34:36])[0] == 68
        and struct.unpack("!H", frame[36:38])[0] == 67
    )


def is_flood(frame):
    return ethertype(frame) == 0x0800 and 54 <= len(frame) <= 60


def is_arp(frame):
    return ethertype(frame) == 0x0806 and len(frame) >= 42


def select_row(bridge, index):
    """Move to row `index` and press OK.

    The submenu always opens on row 0, and usb_ethernet_menu() runs exactly one
    tool before it returns and stops the bridge - so every tool gets a freshly
    opened page, never a second selection on a dead one.

    Keys are spaced widely on purpose: each press redraws the menu, and a press
    only survives ~75 ms before the input task wipes unrenewed flags. Presses
    sent mid-redraw are lost (bench: tightly spaced keys moved nothing, 450 ms
    apart every one landed).
    """
    for _ in range(index):
        bridge.key("down", gap=0.45)
    bridge.key("ok", gap=0.45)


def main():
    if os.geteuid() != 0:
        print("root is required (veth + AF_PACKET): sudo python3 ...")
        return 2
    if not ensure_console():
        print("FAIL: the board does not answer on the console and its page will not close")
        return 1

    if not veth_up():
        return 1

    lan = None
    results = {}
    consoles = {}
    try:
        lan = FakeLan(VD_SERVER)
        threading.Thread(target=lan.run, daemon=True).start()

        def open_page():
            """Ethernet -> USB Ethernet, lease taken, submenu drawn."""
            if not ensure_console():
                print("  cannot reach the board's console")
                return None
            eth_idx = pick(menu(), "ethernet")
            if eth_idx is None:
                print("  no Ethernet entry - is Developer Options on?")
                return None
            console(f"option {eth_idx}")
            time.sleep(0.6)
            sub = menu()
            bridge_idx = pick(sub, "usb ethernet")
            if bridge_idx is None:
                print(f"  no USB Ethernet entry in {[o['label'] for o in (sub or {}).get('options', [])]}")
                return None

            lan.board_mac = None
            lan.offered = lan.acked = False
            console(f"option {bridge_idx}", timeout=0.4)  # console suspends here
            br = Bridge(VD_BRIDGE, PORT)
            br.open()
            for loop in (br.rx_loop, br.wire_loop, br.link_loop):
                threading.Thread(target=loop, daemon=True).start()
            deadline = time.time() + 25
            while time.time() < deadline and not lan.acked:
                time.sleep(0.15)
            if not lan.acked:
                print("  the board never took a DHCP lease over the bridge")
                br.close()
                return None
            time.sleep(2.0)  # submenu drawn
            return br

        def close_page(br):
            """Stop the tool and get the console back.

            The stop frame is repeated: between them the page may still be in a
            blocking step (the scanner's gateway ping, a host list), and the
            board only acts on it once that step polls the stop request.
            """
            br.close()
            time.sleep(1.0)
            for _ in range(5):
                if menu() is not None:
                    return True
                send_stop_frame()
                time.sleep(1.2)
            return menu() is not None

        def wait_started(mark, want, timeout=4.0):
            deadline = time.time() + timeout
            while time.time() < deadline:
                if any(want(f) for f in lan.since(mark)):
                    return True
                time.sleep(0.2)
            return False

        # ---- DHCP starvation -------------------------------------------
        print("\nDHCP Starvation")
        br = open_page()
        if br is None:
            results["starvation"] = False
        else:
            mark = lan.mark()
            select_row(br, 1)
            if not wait_started(mark, is_discover):
                print("  FAIL: no DISCOVER left the board")
                results["starvation"] = False
            else:
                time.sleep(2.0)
                discovers = [f for f in lan.since(mark) if is_discover(f)]
                macs = {src_mac(f) for f in discovers}
                bad = {}
                for f in discovers:
                    for problem in dhcp_discover_problems(f):
                        bad[problem] = bad.get(problem, 0) + 1
                print(f"  {len(discovers)} DISCOVER frames, {len(macs)} distinct source MACs")
                if bad:
                    print("  frame problems: " + ", ".join(f"{k} x{v}" for k, v in sorted(bad.items())))
                else:
                    print("  every DISCOVER is valid (cookie, option 53, chaddr, checksum, unicast MAC)")
                br.key("ok")  # any key stops the loop
                time.sleep(0.7)
                after = lan.mark()
                time.sleep(0.8)
                still = len(lan.since(after))
                print(f"  stopped from the PC; {still} frames afterwards")
                results["starvation"] = bool(discovers) and not bad and still == 0
            consoles["starvation"] = close_page(br)

        # ---- MAC flooding ----------------------------------------------
        print("\nMAC Flooding")
        br = open_page()
        if br is None:
            results["flood"] = False
        else:
            mark = lan.mark()
            select_row(br, 2)
            if not wait_started(mark, is_flood):
                print("  FAIL: no flood left the board")
                results["flood"] = False
            else:
                time.sleep(2.0)
                flood = [f for f in lan.since(mark) if is_flood(f)]
                macs = {src_mac(f) for f in flood}
                multicast = sum(1 for f in flood if not is_unicast(src_mac(f)))
                print(f"  {len(flood)} flooded frames, {len(macs)} distinct source MACs")
                print(f"  multicast/broadcast source MACs: {multicast}")
                br.key("ok")
                time.sleep(0.7)
                after = lan.mark()
                time.sleep(0.8)
                still = len(lan.since(after))
                print(f"  stopped from the PC; {still} frames afterwards")
                # Every frame must carry a unicast source MAC: that is the only
                # thing a switch learns into its CAM table, so a multicast source
                # is a wasted frame, not a second opinion.
                results["flood"] = len(flood) > 200 and len(macs) > 200 and multicast == 0 and still == 0
            consoles["flood"] = close_page(br)

        # ---- scan hosts -------------------------------------------------
        print("\nScan Hosts")
        br = open_page()
        if br is None:
            results["scan"] = False
        else:
            mark = lan.mark()
            select_row(br, 0)
            if not wait_started(mark, is_arp):
                print("  FAIL: no ARP request left the board")
                results["scan"] = False
            else:
                time.sleep(3.0)
                arps = [
                    f
                    for f in lan.since(mark)
                    if is_arp(f) and struct.unpack("!H", f[20:22])[0] == 1 and f[28:32] == ip_bytes(BOARD_IP)
                ]
                targets = {".".join(str(b) for b in f[38:42]) for f in arps}
                host_hit = any(f[38:42] == ip_bytes(HOST_IP) for f in arps)
                print(f"  {len(arps)} ARP requests, {len(targets)} distinct targets")
                print(f"  the fake host {HOST_IP} was probed: {host_hit}")
                br.key("back")  # breaks the scan
                time.sleep(0.8)
                results["scan"] = len(targets) >= 20 and host_hit
            consoles["scan"] = close_page(br)
    finally:
        if lan is not None:
            lan.close()
        time.sleep(1.0)
        veth_down()

    print("")
    for name in ("starvation", "flood", "scan"):
        print(f"{name:12}: {'ok' if results.get(name) else 'FAILED'}")
    print(f"console back: {'yes' if all(consoles.values()) and consoles else 'no'}")
    ok = all(results.get(n) for n in ("starvation", "flood", "scan")) and all(consoles.values())
    print("PASS: all three tools reach the wire, are valid, and stop from the PC" if ok else "FAIL: see above")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
