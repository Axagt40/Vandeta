#!/usr/bin/env python3
"""Host test for the "Mirror image to TV" flows in Vandeta_IR_Suite.js.

Why this exists
---------------
The mirror feature is mostly *protocol*, and protocol mistakes are silent: a
DLNA renderer answers 200 with an error body, a Cast receiver closes the socket,
and the user only sees "nothing happened". Hardware-in-the-loop tests cannot
cover this (no TV, no Chromecast), so this test runs the real JavaScript from
src/modules/ir/ir_suite_src/Vandeta_IR_Suite.js under Node against

  * a mock UPnP MediaRenderer (SSDP is bypassed - it is firmware side - the
    script is handed the renderer's control URL the way wifi.ssdp() would), and
  * stubs for display/keyboard/dialog/storage/ir, so the script's own control
    flow (pick image -> search -> choose -> push) runs for real.

What it checks
--------------
  1. the SOAP envelope is well formed and carries the AVTransport action the
     renderer expects, with a SOAPACTION header of the right shape;
  2. SetAVTransportURI names the served image URL and describes it as
     object.item.imageItem.photo with protocolInfo image/jpeg, which is what a
     photo-capable renderer needs to accept it;
  3. Play follows with InstanceID 0;
  4. the script stops its image server afterwards and reports the TV it used;
  5. the Cast path builds the three CASTV2 messages the protocol requires
     (CONNECT / LAUNCH of CC1AD845 / LOAD with contentId + contentType) - this
     part is a source check, because the sender itself is C++ (cast_js.cpp) and
     only a device can run it.

Run:  python3 FREEBUFF/tests/t31_mirror_protocols.py
"""

import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BADIR = os.path.join(ROOT, "src", "modules", "ir", "ir_suite_src", "Vandeta_IR_Suite.js")
CAST_SRC = os.path.join(ROOT, "src", "modules", "bjs_interpreter", "cast_js.cpp")
STDLIB = os.path.join(ROOT, "src", "modules", "bjs_interpreter", "mqjs_stdlib.c")

checks = 0
failures = []


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print("FAIL  %s" % what)


# ---------------------------------------------------------------------------
# Mock renderer: description + AVTransport control endpoint
# ---------------------------------------------------------------------------
class Renderer:
    def __init__(self):
        self.calls = []
        self.port = 0
        self.fail_set_uri = False

    def description(self):
        return (
            '<?xml version="1.0"?><root xmlns="urn:schemas-upnp-org:device-1-0">'
            "<device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
            "<friendlyName>Mock TV 55&quot;</friendlyName>"
            "<serviceList><service>"
            "<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
            "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
            "<controlURL>/upnp/control/AVTransport1</controlURL>"
            "<eventSubURL>/upnp/event/AVTransport1</eventSubURL>"
            "<SCPDURL>/AVTransport.xml</SCPDURL>"
            "</service></serviceList></device></root>"
        )


def start_renderer():
    renderer = Renderer()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _send(self, body, ctype="text/xml", status=200):
            data = body.encode()
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            self._send(renderer.description())

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length).decode("utf-8", "replace")
            action = (self.headers.get("SOAPACTION") or "").strip('"')
            renderer.calls.append(
                {
                    "path": self.path,
                    "soapaction": action,
                    "content_type": self.headers.get("Content-Type"),
                    "body": body,
                }
            )
            if renderer.fail_set_uri and action.endswith("#SetAVTransportURI"):
                self._send(
                    '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
                    "<s:Body><s:Fault><faultcode>s:Client</faultcode>"
                    "<faultstring>UPnPError</faultstring></s:Fault></s:Body></s:Envelope>",
                    status=500,
                )
                return
            self._send(
                '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
                "<s:Body><u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"/>"
                "</s:Body></s:Envelope>" % (action.split("#")[-1] if "#" in action else "Action")
            )

    httpd = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    renderer.port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return renderer, httpd


# ---------------------------------------------------------------------------
# Node harness: the real script, stubbed device, real HTTP to the mock
# ---------------------------------------------------------------------------
HARNESS = r"""
const fs = require('fs');
const vm = require('vm');
const { execFileSync } = require('child_process');

const source = fs.readFileSync(process.env.BADIR, 'utf8');
const boot = source.indexOf('// start\n// ---');
const body = boot > 0 ? source.slice(0, boot) : source;

let clock = 0;
const logs = [];
const responses = [];

// httpFetch is synchronous in the interpreter; curl keeps the harness honest
// about that instead of awaiting something the firmware never awaits.
function httpFetch(url, options) {
  const args = ['-s', '-w', '\n%{http_code}', '-X', (options && options.method) || 'GET'];
  const headers = (options && options.headers) || [];
  for (let i = 0; i + 1 < headers.length; i += 2) args.push('-H', headers[i] + ': ' + headers[i + 1]);
  if (options && options.body) args.push('--data-binary', options.body);
  args.push(url);
  let out;
  try {
    out = execFileSync('curl', args, { encoding: 'utf8' });
  } catch (e) {
    responses.push({ url: url, error: String(e.status) + ' ' + String(e.message).slice(0, 120) });
    return { status: 0, body: '', headers: {} };
  }
  const cut = out.lastIndexOf('\n');
  const status = parseInt(out.slice(cut + 1), 10);
  const text = out.slice(0, cut);
  responses.push({ url: url, status: status, body: text });
  return { status: status, body: text, headers: {} };
}

const sandbox = {
  display: {
    width: () => 320,
    height: () => 170,
    color: () => 0,
    fill: () => {},
    drawFillRect: () => {},
    drawFillRoundRect: () => {},
    drawString: () => {},
    setTextColor: () => {},
    setTextSize: () => {},
    setTextAlign: () => {},
  },
  keyboard: {
    getAnyPress: () => false,
    getSelPress: () => false,
    getEscPress: () => false,
    getPrevPress: () => false,
    getNextPress: () => false,
    keyboard: () => 'BruceTV',
  },
  dialog: {
    info: (a, b) => logs.push(['info', a, b]),
    error: (a, b) => logs.push(['error', a, b]),
    warning: (a, b) => logs.push(['warning', a, b]),
    choice: (choices) => (Array.isArray(choices) ? choices[0] : ''),
    pickFile: () => process.env.IMG_PATH || '/BruceIR/images/musor.jpg',
  },
  storage: {
    exists: () => true,
    read: () => 'DELAY 10\nINPUT\n',
    write: () => true,
    mkdir: () => true,
    readdir: () => [],
  },
  ir: {
    transmitButton: () => true,
    universalSend: () => true,
    universalCode: () => '',
    universalFamily: () => [],
    universalButtons: () => [],
  },
  wifi: {
    connected: () => true,
    connectDialog: () => true,
    ssdp: () => [process.env.SSDP],
    mdnsCast: () => [],
    serveFile: () => process.env.IMG_URL,
    serveStop: () => logs.push(['serveStop']),
    castImage: () => 'ok',
    apStart: () => '192.168.4.1',
    apStop: () => {},
    httpFetch: httpFetch,
  },
  now: () => (clock += 10),
  delay: () => {},
  console: console,
  process: process,
  require: () => ({}),
};
sandbox.global = sandbox;

vm.createContext(sandbox);
try {
  vm.runInContext(body, sandbox, { filename: 'Vandeta_IR_Suite.js' });
} catch (e) {
  console.log(JSON.stringify({ error: String(e) }));
  process.exit(0);
}

// the sweep/pad code must not run at load time
sandbox.selectedRemote = '/BruceIR/images/remote.ir';
try {
  sandbox.mirrorDlna(false);
} catch (e) {
  logs.push(['exception', String(e)]);
}
console.log(JSON.stringify({ logs: logs, responses: responses }));
"""


def run_harness(renderer, image_url, image_path="/BruceIR/images/musor.jpg"):
    node = shutil.which("node")
    if node is None:
        return None
    tmp = tempfile.NamedTemporaryFile("w", suffix=".js", delete=False)
    tmp.write(HARNESS)
    tmp.close()
    env = dict(os.environ)
    env["BADIR"] = BADIR
    env["IMG_URL"] = image_url
    env["IMG_PATH"] = image_path
    env["SSDP"] = "127.0.0.1|Mock TV 55|http://127.0.0.1:%d/upnp/control/AVTransport1" % renderer.port
    try:
        out = subprocess.run(
            [node, tmp.name], capture_output=True, text=True, timeout=90, env=env
        )
        if out.returncode != 0:
            print(out.stderr[-2000:], file=sys.stderr)
            return None
        return json.loads(out.stdout.strip().splitlines()[-1])
    finally:
        os.unlink(tmp.name)


def main():
    for path in (BADIR, CAST_SRC, STDLIB):
        if not os.path.exists(path):
            print("missing file: %s" % path, file=sys.stderr)
            return 2

    renderer, httpd = start_renderer()
    control = "http://127.0.0.1:%d/upnp/control/AVTransport1" % renderer.port
    image_url = "http://192.168.1.50:8765/musor.jpg"

    check(shutil.which("node") is not None, "node is available for the JS harness")
    result = run_harness(renderer, image_url)
    check(result is not None, "the mirror flow ran without throwing")

    if result:
        check("exception" not in [entry[0] for entry in result["logs"]], "no exception inside mirrorDlna()")
        kinds = [entry[0] for entry in result["logs"]]
        check("serveStop" in kinds, "the image server is stopped when the push is over")
        infos = [entry for entry in result["logs"] if entry[0] == "info"]
        check(bool(infos), "the flow ends on a result dialog for the user")
        if infos:
            check(
                "Mock TV 55" in (infos[-1][2] or ""),
                "the result names the renderer that accepted the image (%r)" % (infos[-1][2],),
            )

    actions = [call["soapaction"].split("#")[-1] for call in renderer.calls]
    check("SetAVTransportURI" in actions, "SetAVTransportURI reached the renderer (%s)" % actions)
    check("Play" in actions, "Play reached the renderer (%s)" % actions)
    check(
        "Stop" in actions
        and "SetAVTransportURI" in actions
        and actions.index("Stop") < actions.index("SetAVTransportURI"),
        "Stop clears a transport left locked by an earlier push before the new URI",
    )

    seturi = [c for c in renderer.calls if c["soapaction"].endswith("#SetAVTransportURI")]
    if seturi:
        call = seturi[0]
        body = call["body"]
        check(call["path"] == "/upnp/control/AVTransport1", "the control URL path is the one from the description")
        check(
            (call["content_type"] or "").startswith("text/xml"),
            "the SOAP request declares text/xml (%r)" % (call["content_type"],),
        )
        check("<InstanceID>0</InstanceID>" in body, "the SOAP body targets InstanceID 0")
        check(image_url in body, "the SOAP body carries the served image URL")
        check(
            "object.item.imageItem.photo" in body,
            "the DIDL metadata declares a photo item (some renderers refuse otherwise)",
        )
        check(
            "http-get:*:image/jpeg:*" in body,
            "the DIDL res entry declares the jpeg protocol info",
        )
        check(
            body.startswith("<?xml") and "<s:Envelope" in body and "</s:Envelope>" in body,
            "the envelope is a complete SOAP 1.1 message",
        )

    play = [c for c in renderer.calls if c["soapaction"].endswith("#Play")]
    if play:
        check("<Speed>1</Speed>" in play[0]["body"], "Play sends a speed, which some renderers require")

    # A renderer that refuses the item must not be reported as a success. The
    # Sony KD-49XG7077 answers 500 to SetAVTransportURI and 200 to Play, which
    # made the old "okUri || okPlay" return true and show "Sent".
    renderer.fail_set_uri = True
    renderer.calls.clear()
    refused = run_harness(renderer, image_url)
    check(refused is not None, "the refused-item flow ran without throwing")
    if refused:
        kinds = [entry[0] for entry in refused["logs"]]
        check("error" in kinds, "a 500 from SetAVTransportURI ends in an error dialog")
        check("info" not in kinds, "a 500 from SetAVTransportURI is not reported as Sent")
        check(
            "Play" not in [call["soapaction"].split("#")[-1] for call in renderer.calls],
            "Play is not sent after the renderer refused the item",
        )

    # A video is a different DIDL item; the Sony that refuses a still JPEG
    # answered 200/200 and stayed PLAYING for this MP4.
    renderer.fail_set_uri = False
    renderer.calls.clear()
    video_url = "http://192.168.1.50:8765/musor.mp4"
    video = run_harness(renderer, video_url, "/BruceIR/images/musor.mp4")
    check(video is not None, "the MP4 flow ran without throwing")
    if video:
        kinds = [entry[0] for entry in video["logs"]]
        check("exception" not in kinds, "no exception inside the MP4 mirrorDlna()")
        check("error" not in kinds, "the MP4 push is not reported as a failure")
        video_set = [
            call for call in renderer.calls if call["soapaction"].endswith("#SetAVTransportURI")
        ]
        check(bool(video_set), "the MP4 reached SetAVTransportURI")
        if video_set:
            body = video_set[0]["body"]
            check("object.item.videoItem" in body, "the MP4 DIDL declares a video item")
            check("http-get:*:video/mp4:*" in body, "the MP4 DIDL declares video/mp4")

    httpd.shutdown()

    # ---- Cast: the sender is C++, so this part is a source-level contract ----
    cast = open(CAST_SRC, "r", encoding="utf-8").read()
    check("CC1AD845" in cast, "Cast launches the Default Media Receiver app id")
    check('urn:x-cast:com.google.cast.receiver' in cast, "LAUNCH goes to the receiver namespace")
    check('urn:x-cast:com.google.cast.media' in cast, "LOAD goes to the media namespace")
    check(
        "pbFieldString(msg, 6, json)" in cast and "pbFieldString(msg, 2, \"sender-0\")" in cast,
        "the CASTV2 frame is a protobuf CastMessage (sourceId + payloadUtf8)",
    )
    check(
        '\\"contentId\\":\\"' in cast and "\\\"contentType\\\"" in cast,
        "LOAD carries contentId and contentType",
    )
    check("client.setInsecure()" in cast, "the Cast link skips certificate pinning (LAN device, CA not in flash)")
    check("8009" in cast, "the Cast port is the standard one")
    # The image itself must come from RAM, not from the SD card inside the
    # AsyncTCP task: streaming from SD stalled after a few KB and the TV got a
    # truncated JPEG ("Playback not available").
    check("castFileData" in cast and "heap_caps_malloc" in cast,
          "serveFile buffers the image in PSRAM instead of streaming it from SD")
    check("contentFeatures.dlna.org" in cast and "transferMode.dlna.org" in cast,
          "the image server sends the DLNA headers a renderer checks")

    stdlib = open(STDLIB, "r", encoding="utf-8").read()
    for name in ("ssdp", "mdnsCast", "serveFile", "serveStop", "apStart", "apStop", "castImage"):
        check(
            'JS_CFUNC_DEF("%s"' % name in stdlib,
            "wifi.%s is registered in the JS stdlib" % name,
        )

    js = open(BADIR, "r", encoding="utf-8").read()
    check("wifi.apStart(ssid, \"\", 6)" in js, "the password-protected-TV path opens an AP with no password")
    check(
        "open network - no password" in js,
        "the AP screen tells the user the network is open (so the TV needs no keyboard)",
    )
    check(
        js.count("mirrorEnsureNetwork") >= 3,
        "both push paths re-use the same network check",
    )
    check(
        "wifi.serveStop()" in js,
        "the script always stops the image server, no matter which path ran",
    )

    print()
    print("%d checks, %d failures" % (checks, len(failures)))
    for failure in failures:
        print("  - %s" % failure)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
