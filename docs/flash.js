/* Browser flasher: Web Serial + esptool-js (pinned). All checks before writing. */
const FW_URL = "firmware/vandeta.bin";
const SHA_URL = "firmware/vandeta.bin.sha256";
const ESPTOOL_URL = "https://unpkg.com/esptool-js@0.7.0/bundle.js";

const el = (id) => document.getElementById(id);
const logEl = el("log");
const fwBadge = el("fw-state");
const fwName = el("fw-name");
const fwSize = el("fw-size");
const fwSha = el("fw-sha");
const fwExpected = el("fw-expected");
const connectBtn = el("connect-btn");
const flashBtn = el("flash-btn");
const disconnectBtn = el("disconnect-btn");
const eraseBox = el("erase-all");
const connStatus = el("conn-status");
const chipEl = el("chip");
const progressBar = el("progress");
const progressPct = el("progress-pct");
const progressNote = el("progress-note");
const baudSelect = el("baud");

let firmware = null; // Uint8Array
let expectedSha = null; // hex string
let port = null;
let transport = null;
let loader = null;
let busy = false;

function log(line) {
  if (!logEl) return;
  logEl.textContent += line + "\n";
  const lines = logEl.textContent.split("\n");
  if (lines.length > 600) logEl.textContent = lines.slice(-500).join("\n");
  logEl.scrollTop = logEl.scrollHeight;
}

function setStatus(text, kind) {
  connStatus.textContent = text;
  connStatus.className = "status" + (kind ? " " + kind : "");
}

function hex(buffer) {
  return Array.from(new Uint8Array(buffer))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

async function sha256(bytes) {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return hex(digest);
}

function humanSize(n) {
  if (n >= 1048576) return (n / 1048576).toFixed(2) + " MB (" + n.toLocaleString() + " bytes)";
  return n.toLocaleString() + " bytes";
}

function setBadge(text, cls) {
  fwBadge.textContent = text;
  fwBadge.className = "badge " + (cls || "");
}

function updateFlashButton() {
  const ready = Boolean(firmware && loader && !busy);
  flashBtn.disabled = !ready;
  flashBtn.setAttribute("aria-disabled", String(!ready));
}

async function loadFirmware() {
  setBadge("loading", "");
  try {
    const [binResponse, shaResponse] = await Promise.all([
      fetch(FW_URL, { cache: "no-store" }),
      fetch(SHA_URL, { cache: "no-store" }),
    ]);
    if (!binResponse.ok) throw new Error("cannot download " + FW_URL + " (" + binResponse.status + ")");
    if (!shaResponse.ok) throw new Error("cannot read " + SHA_URL + " (" + shaResponse.status + ")");
    const text = await shaResponse.text();
    const match = text.trim().split(/\s+/)[0];
    if (!/^[0-9a-f]{64}$/i.test(match)) throw new Error("checksum file is not a sha256 line");
    expectedSha = match.toLowerCase();
    fwExpected.textContent = expectedSha;

    const bytes = new Uint8Array(await binResponse.arrayBuffer());
    await acceptFirmware(bytes, FW_URL);
    if (firmware) {
      setStatus("Firmware verified. Connect the board to continue.", "meta-ok");
    }
  } catch (error) {
    setBadge("unavailable", "meta-bad");
    fwName.textContent = FW_URL;
    fwSize.textContent = "could not load";
    fwSha.textContent = String(error.message || error);
    log("firmware: " + (error.message || error));
    setStatus("Firmware could not be loaded. Serve this page over http(s), not file://.", "meta-bad");
  }
}

async function acceptFirmware(bytes, sourceName) {
  fwName.textContent = sourceName;
  fwSize.textContent = humanSize(bytes.length);
  const digest = await sha256(bytes);
  fwSha.textContent = digest;
  if (expectedSha && digest !== expectedSha) {
    firmware = null;
    setBadge("wrong image", "meta-bad");
    log(
      "refusing: " + sourceName + " hashes to " + digest + ", but this page expects " + expectedSha +
      ". Download the image from the release page instead of flashing a different build."
    );
    updateFlashButton();
    return false;
  }
  firmware = bytes;
  setBadge("verified", "meta-ok");
  log("firmware: " + sourceName + " · " + humanSize(bytes.length) + " · sha256 " + digest);
  updateFlashButton();
  return true;
}

async function loadEsptool() {
  const module = await import(ESPTOOL_URL);
  if (!module.ESPLoader || !module.Transport) throw new Error("esptool-js loaded but exports are missing");
  return module;
}

function terminal() {
  let pending = "";
  return {
    clean() {},
    writeLine(data) {
      if (data) log(data);
    },
    write(data) {
      pending += String(data);
      const parts = pending.split("\n");
      pending = parts.pop();
      parts.forEach((line) => log(line));
    },
  };
}

async function connect() {
  if (!("serial" in navigator)) {
    setStatus("This browser has no Web Serial API. Use Chrome or Edge 89+ on desktop, over https or localhost.", "meta-bad");
    return;
  }
  connectBtn.disabled = true;
  setStatus("Waiting for a serial port…");
  try {
    let selected;
    try {
      selected = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303a }] });
    } catch (error) {
      if (error && error.name === "NotFoundError") {
        log("no Espressif device in the filtered list; showing every serial port");
        selected = await navigator.serial.requestPort();
      } else {
        throw error;
      }
    }

    const info = selected.getInfo ? selected.getInfo() : {};
    log(
      "port: vendor " + (info.usbVendorId !== undefined ? "0x" + info.usbVendorId.toString(16) : "?") +
      ", product " + (info.usbProductId !== undefined ? "0x" + info.usbProductId.toString(16) : "?")
    );
    if (info.usbProductId === 0x0002) {
      throw new Error("the board is in USB Mass Storage mode (303a:0002). Leave that mode on the device, then retry.");
    }
    if (info.usbVendorId !== undefined && info.usbVendorId !== 0x303a) {
      throw new Error("this is not an Espressif USB device. Vandeta flashes over the board's own USB-Serial-JTAG port (303a:1001).");
    }

    const module = await loadEsptool();
    port = selected;
    transport = new module.Transport(port, false);
    const baud = Number(baudSelect.value) || 115200;
    loader = new module.ESPLoader({ transport, baudrate: baud, terminal: terminal() });
    log("connecting at " + baud + " baud…");
    const chip = await loader.main();
    chipEl.textContent = chip;
    log("connected: " + chip);
    if (!/ESP32-S3/i.test(chip)) {
      throw new Error("this image is built for the ESP32-S3; the connected chip is " + chip);
    }
    setStatus("Connected. Ready to flash.", "meta-ok");
    disconnectBtn.disabled = false;
    updateFlashButton();
  } catch (error) {
    log("connect failed: " + (error.message || error));
    setStatus(String(error.message || error), "meta-bad");
    await teardown();
  } finally {
    connectBtn.disabled = false;
  }
}

async function teardown() {
  try {
    if (transport) await transport.disconnect();
  } catch (error) {
    /* port may already be gone */
  }
  transport = null;
  loader = null;
  port = null;
  chipEl.textContent = "—";
  disconnectBtn.disabled = true;
  updateFlashButton();
}

async function disconnect() {
  setStatus("Disconnected.");
  log("disconnected");
  await teardown();
}

async function flash() {
  if (!loader || !firmware || busy) return;
  busy = true;
  updateFlashButton();
  connectBtn.disabled = true;
  disconnectBtn.disabled = true;
  progressBar.style.width = "0%";
  progressPct.textContent = "0%";
  progressNote.textContent = eraseBox.checked ? "erasing, then writing 0x0…" : "writing at 0x0…";
  setStatus("Flashing. Do not unplug the cable.", "");
  try {
    await loader.writeFlash({
      fileArray: [{ data: firmware, address: 0x0 }],
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: eraseBox.checked,
      compress: true,
      reportProgress: (fileIndex, written, total) => {
        const pct = total > 0 ? Math.floor((written / total) * 100) : 0;
        progressBar.style.width = pct + "%";
        progressPct.textContent = pct + "%";
        progressNote.textContent = humanSize(written) + " of " + humanSize(total);
      },
    });
    log("flash complete");
    progressBar.style.width = "100%";
    progressPct.textContent = "100%";
    progressNote.textContent = "done";
    setStatus("Flashed. Resetting the board.", "meta-ok");
    try {
      await loader.after("hard_reset");
    } catch (error) {
      log("reset: " + (error.message || error));
    }
    await teardown();
    setStatus("Done. The board is rebooting into Vandeta.", "meta-ok");
  } catch (error) {
    log("flash failed: " + (error.message || error));
    setStatus("Flash failed: " + (error.message || error), "meta-bad");
    await teardown();
  } finally {
    busy = false;
    connectBtn.disabled = false;
    updateFlashButton();
  }
}

connectBtn.addEventListener("click", connect);
disconnectBtn.addEventListener("click", disconnect);
flashBtn.addEventListener("click", flash);
el("file").addEventListener("change", async (event) => {
  const file = event.target.files && event.target.files[0];
  if (!file) return;
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const ok = await acceptFirmware(bytes, file.name);
    if (!ok) {
      setStatus("That file is not the image this page expects. Nothing will be flashed.", "meta-bad");
    } else {
      setStatus("Image verified. Connect the board to continue.", "meta-ok");
    }
  } catch (error) {
    log("file: " + (error.message || error));
  }
});

if (!("serial" in navigator)) {
  setStatus("Web Serial is not available in this browser. Chrome or Edge 89+ on desktop is required.", "meta-bad");
  connectBtn.disabled = true;
}

log("Ready. Checks run in this order: image hash, port, chip, then flash at 0x0.");
loadFirmware();
