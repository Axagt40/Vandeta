#!/usr/bin/env python3
"""t39 - exercise tools/eth_bridge.py against a real interface.

t38 proves the board side of the bridge by talking to it directly. This one
proves the *host* side: the helper's raw-socket path (promiscuous capture,
destination filtering, injection) plus the board, end to end.

It builds a veth pair, lets the helper bridge the board onto one end, and
answers the board's DHCP from the other end - the same job their switch does on
a real LAN, done here so the test needs no cable and no router.

    sudo python3 tools/tests/t39_eth_bridge_host.py [/dev/ttyACM0]

With real hardware - the hub's RJ45 cabled to this PC's own port - name both
ends and no veth is created:

    sudo python3 tools/tests/t39_eth_bridge_host.py \\
        --bridge-iface enx50a030083945 --server-iface enp1s0

Root is required: `ip link` and AF_PACKET. A veth pair it created is removed
afterwards (named interfaces are never touched), and the board is sent the stop
frame so it returns to its menu.
"""

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "..", "tools", "eth_bridge.py")

def arg_value(flag, default=None):
    if flag in sys.argv:
        i = sys.argv.index(flag)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


_consumed = set()
for _flag in ("--bridge-iface", "--server-iface"):
    if _flag in sys.argv:
        _i = sys.argv.index(_flag)
        _consumed.update({_flag, sys.argv[_i + 1] if _i + 1 < len(sys.argv) else ""})
PORT = [a for a in sys.argv[1:] if not a.startswith("--") and a not in _consumed]
PORT = PORT[0] if PORT else "/dev/ttyACM0"

# Real interfaces: the caller provides both ends, nothing is created or removed.
BRIDGE_IFACE = arg_value("--bridge-iface")
SERVER_IFACE = arg_value("--server-iface")

VD_BRIDGE = BRIDGE_IFACE or "vdb0"  # the helper bridges this one (hub's LAN)
VD_SERVER = SERVER_IFACE or "vdb1"  # this test answers DHCP on its peer

BOARD_IP = "192.168.99.5"
SERVER_IP = "192.168.99.1"
SERVER_MAC = bytes.fromhex("020000000001")
ETH_P_ALL = 0x0003
SOL_PACKET, PACKET_ADD_MEMBERSHIP, PACKET_MR_PROMISC = 263, 1, 1


# ---------------------------------------------------------------- packets


def ip_bytes(text):
    return bytes(int(p) for p in text.split("."))


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
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, total, 0x42, 0, 64, proto, 0, ip_bytes(src), ip_bytes(dst))
    return hdr[:10] + struct.pack("!H", checksum(hdr)) + hdr[12:] + payload


def udp(sport, dport, payload, src, dst):
    length = 8 + len(payload)
    hdr = struct.pack("!HHHH", sport, dport, length, 0)
    pseudo = ip_bytes(src) + ip_bytes(dst) + struct.pack("!BBH", 0, 17, length)
    return struct.pack("!HHHH", sport, dport, length, checksum(pseudo + hdr + payload) or 0xFFFF) + payload


def dhcp_reply(mac, xid, msgtype):
    body = struct.pack("!BBBBIHH", 2, 1, 6, 0, xid, 0, 0x8000)
    body += struct.pack("!IIII", 0, int.from_bytes(ip_bytes(BOARD_IP), "big"), 0, 0)
    body += mac + b"\x00" * 10 + b"\x00" * 64 + b"\x00" * 128
    body += b"\x63\x82\x53\x63"
    body += bytes([53, 1, msgtype]) + bytes([54, 4]) + ip_bytes(SERVER_IP)
    body += bytes([51, 4]) + struct.pack("!I", 120) + bytes([1, 4]) + ip_bytes("255.255.255.0")
    body += bytes([3, 4]) + ip_bytes(SERVER_IP) + b"\xff"
    bcast = "255.255.255.255"
    return eth(b"\xff" * 6, SERVER_MAC, 0x0800, ipv4(SERVER_IP, bcast, 17, udp(67, 68, body, SERVER_IP, bcast)))


def arp_request(target_ip):
    body = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1)
    body += SERVER_MAC + ip_bytes(SERVER_IP) + b"\x00" * 6 + ip_bytes(target_ip)
    return eth(b"\xff" * 6, SERVER_MAC, 0x0806, body)


# ---------------------------------------------------------------- helpers


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def using_real_nics():
    return bool(BRIDGE_IFACE and SERVER_IFACE)


def veth_up():
    if using_real_nics():
        out = sh(f"ip link set {VD_BRIDGE} up && ip link set {VD_SERVER} up")
        if out.returncode != 0:
            print(f"cannot bring interfaces up: {out.stderr.strip()}")
            return False
        print(f"using real NICs: helper on {VD_BRIDGE}, DHCP served on {VD_SERVER}")
        return True
    sh(f"ip link del {VD_BRIDGE} 2>/dev/null")
    out = sh(f"ip link add {VD_BRIDGE} type veth peer name {VD_SERVER}")
    if out.returncode != 0:
        print(f"cannot create veth: {out.stderr.strip()}")
        return False
    sh(f"ip link set {VD_BRIDGE} up")
    sh(f"ip link set {VD_SERVER} up")
    return True


def veth_down():
    # Never delete an interface the caller named - it is their hardware.
    if using_real_nics():
        return
    sh(f"ip link del {VD_BRIDGE} 2>/dev/null")


def open_raw(iface):
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    sock.bind((iface, 0))
    mreq = struct.pack("IHH8s", socket.if_nametoindex(iface), PACKET_MR_PROMISC, 0, b"\x00" * 8)
    sock.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, mreq)
    sock.settimeout(0.05)
    return sock


def console(cmd, timeout=6.0):
    """One developer-console command; used before and after the bridge."""
    import serial

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


def send_bridge_frame(kind, payload=b""):
    """Write one framed message to the board (the helper's side of the wire)."""
    import serial

    ser = serial.Serial(PORT, 115200, timeout=0.2)
    time.sleep(0.2)
    ser.write(b"\xa5\x5a\x5a\xa5" + bytes([kind]) + struct.pack("<H", len(payload)) + payload)
    ser.flush()
    time.sleep(0.1)
    ser.close()


def ensure_console():
    """The board may already be sitting on the USB Ethernet page - it then owns
    the port and the console is suspended, so no menu query is answered. Ask it
    to close with the stop frame (exactly what the helper does on exit)."""
    if menu() is not None:
        return True
    print("board is on the bridge page; sending the stop frame to close it")
    send_bridge_frame(3)
    for _ in range(8):
        time.sleep(0.6)
        if menu() is not None:
            print("  page closed, console is back")
            return True
    return False


def menu(title_cmd="optionsJSON"):
    raw = console(title_cmd)
    for line in reversed(raw.splitlines()):
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


# ---------------------------------------------------------------- test


def main():
    if os.geteuid() != 0:
        print("root is required (veth + AF_PACKET): sudo python3 ...")
        return 2

    if not ensure_console():
        print("FAIL: the board does not answer on the console and its page will not close")
        return 1

    board = menu()
    idx = pick(board, "ethernet")
    if idx is None:
        print("FAIL: no Ethernet entry - is Developer Options on?")
        return 1
    console(f"option {idx}")
    time.sleep(0.6)
    sub = menu()
    bridge_idx = pick(sub, "usb ethernet")
    if bridge_idx is None:
        print(f"FAIL: no USB Ethernet entry in {[o['label'] for o in (sub or {}).get('options', [])]}")
        return 1

    if not veth_up():
        return 1

    try:
        print(f"opening the page (option {bridge_idx}) and starting the helper")
        console(f"option {bridge_idx}", timeout=0.4)  # the console suspends now

        helper = subprocess.Popen(
            [sys.executable, HELPER, "--iface", VD_BRIDGE, "--port", PORT],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

        sock = open_raw(VD_SERVER)
        board_mac = None
        offered = acked = False
        arp_reply = False
        start = time.time()
        last_arp = 0.0
        while time.time() - start < 40.0:
            now = time.time()
            if acked and now - last_arp > 1.0:
                sock.send(arp_request(BOARD_IP))
                last_arp = now
            try:
                frame = sock.recv(2048)
            except socket.timeout:
                continue
            if len(frame) < 42:
                continue
            ethertype = struct.unpack("!H", frame[12:14])[0]
            if ethertype == 0x0800 and frame[23] == 17:
                sport, dport = struct.unpack("!HH", frame[34:38])
                if sport == 68 and dport == 67:
                    body = frame[42:]
                    xid = struct.unpack("!I", body[4:8])[0]
                    mac = body[28:34]
                    mtype = body[242] if len(body) > 242 else None
                    board_mac = board_mac or mac
                    if mtype == 1 and not offered:
                        sock.send(dhcp_reply(mac, xid, 2))
                        offered = True
                        print(f"  helper delivered DISCOVER; OFFER sent for {':'.join(f'{b:02x}' for b in mac)}")
                    elif mtype == 3 and not acked:
                        sock.send(dhcp_reply(mac, xid, 5))
                        acked = True
                        print("  helper delivered REQUEST; ACK sent")
            elif ethertype == 0x0806 and len(frame) >= 42:
                if struct.unpack("!H", frame[20:22])[0] == 2 and frame[28:32] == ip_bytes(BOARD_IP):
                    arp_reply = True
                    print("  helper delivered the board's ARP reply to the wire")
                    break

        time.sleep(1.0)
        helper.send_signal(signal.SIGINT)
        try:
            log = helper.communicate(timeout=8)[0]
        except subprocess.TimeoutExpired:
            helper.kill()
            log = helper.communicate()[0]

        learned = "board MAC" in log
        stats = [line for line in log.splitlines() if "to_wire=" in line]
        print("  helper log:", "; ".join(stats[-1:]) or "(no stats line)")
        print(f"  helper learned the board MAC: {learned}")
        sock.close()

        # The stop frame the helper sends on exit must hand the console back.
        time.sleep(1.5)
        back = menu()
        console_ok = back is not None

        print("")
        print(f"helper -> wire  : {'ok' if arp_reply else 'no'}")
        print(f"wire -> helper  : {'ok' if passed_discover(offered, acked) else 'no'}")
        print(f"console restored: {'yes' if console_ok else 'no'}")
        ok = arp_reply and offered and acked and learned and console_ok
        print("PASS: the helper bridges both directions and the page closes cleanly" if ok else "FAIL: see above")
        return 0 if ok else 1
    finally:
        veth_down()


def passed_discover(offered, acked):
    return offered and acked


if __name__ == "__main__":
    sys.exit(main())
