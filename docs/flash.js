/* Browser flasher for Vandeta.

   The page asks GitHub for the latest release, downloads that tag's image from
   jsDelivr (release assets themselves send no CORS header, the CDN does),
   verifies it against the asset digest GitHub reports AND the published
   .sha256, then writes it at 0x0 with esptool-js and resets the board.

   One button does the whole thing; the port, baud, erase and file picker are in
   Advanced for troubleshooting. */
const REPO = "Axagt40/Vandeta";
const RELEASE_API = "https://api.github.com/repos/" + REPO + "/releases/latest";
const ESPTOOL_URL = "https://unpkg.com/esptool-js@0.7.0/bundle.js";
const LOCAL_BIN = "firmware/vandeta.bin";
const LOCAL_SHA = "firmware/vandeta.bin.sha256";
const raw = (ref, file) => "https://raw.githubusercontent.com/" + REPO + "/" + ref + "/" + file;

const el = (id) => document.getElementById(id);
const logEl = el("log");
const badge = el("fw-state");
const nameEl = el("version-name");
const tagEl = el("version-tag");
const dateEl = el("version-date");
const sizeEl = el("fw-size");
const shaEl = el("fw-sha");
const expectedEl = el("fw-expected");
const sourceEl = el("fw-source");
const installBtn = el("install-btn");
const refreshBtn = el("refresh-btn");
const connectBtn = el("connect-btn");
const disconnectBtn = el("disconnect-btn");
const eraseBox = el("erase-all");
const baudSelect = el("baud");
const statusEl = el("conn-status");
const chipEl = el("chip");
const progressBar = el("progress");
const progressPct = el("progress-pct");
const progressNote = el("progress-note");

let release = null; // { name, tag, size, digest, published, binUrl, shaUrl, local }
let bytes = null; // verified image
let expectedSha = null;
let port = null;
let transport = null;
let loader = null;
let busy = false;

function log(line) {
  if (!logEl) return;
  logEl.textContent += line + "\n";
  const keep = logEl.textContent.split("\n").slice(-500).join("\n");
  logEl.textContent = keep;
  logEl.scrollTop = logEl.scrollHeight;
}

function setStatus(text, kind) {
  statusEl.textContent = text;
  statusEl.className = "status" + (kind ? " " + kind : "");
}

function setBadge(text, cls) {
  badge.textContent = text;
  badge.className = "badge " + (cls || "");
}

function hex(buffer) {
  return Array.from(new Uint8Array(buffer))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

async function sha256(bytesIn) {
  return hex(await crypto.subtle.digest("SHA-256", bytesIn));
}

function humanSize(n) {
  if (!n) return "—";
  return (n / 1048576).toFixed(2) + " MB (" + n.toLocaleString() + " bytes)";
}

function updateInstallButton() {
  const label = release ? "Install " + release.name : "Install Vandeta";
  installBtn.textContent = label;
  const ready = Boolean(release && !busy && "serial" in navigator);
  installBtn.disabled = !ready;
  installBtn.setAttribute("aria-disabled", String(!ready));
}

async function readSidecarSha(url) {
  const response = await fetch(url, { cache: "no-store" });
  if (!response.ok) throw new Error("checksum file " + response.status);
  const text = await response.text();
  const token = text.trim().split(/\s+/)[0] || "";
  if (!/^[0-9a-f]{64}$/i.test(token)) throw new Error("checksum file is not a sha256 line");
  return token.toLowerCase();
}

// A release tag snapshots the repository as it was when the tag was made, so a
// tag's copy of the image can lag the release asset. Try the tag, then main,
// then the copy in this site; the digest check decides, not the URL.
async function findSidecarSha(tag) {
  const candidates = [
    raw(tag, "vandeta.bin.sha256"),
    raw("main", "vandeta.bin.sha256") + "?t=" + Date.now(),
    LOCAL_SHA,
  ];
  for (const url of candidates) {
    try {
      return await readSidecarSha(url);
    } catch (error) {
      /* try the next one */
    }
  }
  throw new Error("no usable .sha256 found");
}

async function loadRelease() {
  setBadge("checking", "");
  nameEl.textContent = "Checking GitHub…";
  bytes = null;
  shaEl.textContent = "checked when it downloads";
  try {
    const response = await fetch(RELEASE_API + "?t=" + Date.now(), {
      headers: { Accept: "application/vnd.github+json" },
      cache: "no-store",
    });
    if (!response.ok) throw new Error("GitHub API answered " + response.status);
    const data = await response.json();
    const asset = (data.assets || []).find((a) => a.name === "vandeta.bin");
    if (!asset) throw new Error("the latest release has no vandeta.bin");
    const tag = data.tag_name;
    release = {
      name: data.name || tag,
      tag,
      size: asset.size,
      digest: String(asset.digest || "").replace(/^sha256:/, "").toLowerCase(),
      published: data.published_at,
      sources: [
        { url: raw(tag, "vandeta.bin"), label: "release tag " + tag },
        { url: raw("main", "vandeta.bin") + "?t=" + Date.now(), label: "repository main" },
        { url: LOCAL_BIN, label: "bundled with this site" },
      ],
      local: false,
    };
    expectedSha = release.digest || (await findSidecarSha(tag));
    if (release.digest) {
      findSidecarSha(tag)
        .then((sidecar) => {
          if (sidecar !== release.digest) {
            log("note: the tag's .sha256 is older than the release asset; using the asset digest");
          }
        })
        .catch(() => {});
    }
    renderRelease();
    setStatus("Ready. The newest release will be downloaded and verified.", "");
    log("latest release: " + release.name + " (" + release.tag + "), " + humanSize(release.size));
  } catch (error) {
    log("latest release: " + (error.message || error) + " — falling back to the copy shipped with this site");
    await loadLocal();
  }
}

async function loadLocal() {
  try {
    expectedSha = await readSidecarSha(LOCAL_SHA);
    const head = await fetch(LOCAL_BIN, { method: "HEAD", cache: "no-store" });
    if (!head.ok) throw new Error("bundled image is missing (" + head.status + ")");
    release = {
      name: "Vandeta (bundled copy)",
      tag: "bundled",
      size: Number(head.headers.get("content-length")) || 0,
      digest: expectedSha,
      published: "",
      sources: [{ url: LOCAL_BIN, label: "bundled with this site" }],
      local: true,
    };
    renderRelease();
    setStatus("Offline copy. GitHub could not be reached, so this is the image in this site.", "warn");
  } catch (error) {
    setBadge("unavailable", "meta-bad");
    nameEl.textContent = "No image available";
    log("bundled image: " + (error.message || error));
    setStatus("No firmware could be loaded. Check the connection and press Check for updates.", "meta-bad");
  }
}

function renderRelease() {
  nameEl.textContent = release.name;
  tagEl.textContent = release.tag;
  dateEl.textContent = release.published ? release.published.slice(0, 10) : "—";
  sizeEl.textContent = humanSize(release.size);
  expectedEl.textContent = expectedSha;
  sourceEl.textContent = release.local ? "bundled with this site" : "GitHub release " + release.tag;
  setBadge(release.local ? "offline copy" : "latest release", "meta-ok");
  updateInstallButton();
}

async function downloadAndVerify() {
  if (!release) throw new Error("no release loaded");
  for (const source of release.sources) {
    try {
      setStatus("Downloading " + release.name + " from " + source.label + "…", "");
      progressNote.textContent = "downloading";
      const response = await fetch(source.url, { cache: "no-store" });
      if (!response.ok) throw new Error("HTTP " + response.status);
      const buffer = new Uint8Array(await response.arrayBuffer());
      progressNote.textContent = "verifying";
      const digest = await sha256(buffer);
      shaEl.textContent = digest;
      log("downloaded " + humanSize(buffer.length) + " from " + source.label + ", sha256 " + digest);
      if (digest !== expectedSha) {
        log("skipped " + source.label + ": hash does not match the release (" + expectedSha.slice(0, 12) + "…)");
        continue;
      }
      if (release.size && buffer.length !== release.size) {
        log("skipped " + source.label + ": " + buffer.length + " bytes, release says " + release.size);
        continue;
      }
      bytes = buffer;
      shaEl.className = "meta-ok";
      sourceEl.textContent = source.label;
      log("verified and ready to write");
      return bytes;
    } catch (error) {
      log("source " + source.label + ": " + (error.message || error));
    }
  }
  shaEl.className = "meta-bad";
  throw new Error("no published copy matched the release hash " + expectedSha);
}

async function loadEsptool() {
  const module = await import(ESPTOOL_URL);
  if (!module.ESPLoader || !module.Transport) throw new Error("esptool-js exports are missing");
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
    throw new Error("this browser has no Web Serial API; use Chrome or Edge 89+ on desktop");
  }
  setStatus("Choose the board's serial port…", "");
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
    throw new Error("the board is in USB Mass Storage mode (303a:0002); leave that mode on the device");
  }
  if (info.usbVendorId !== undefined && info.usbVendorId !== 0x303a) {
    throw new Error("that is not an Espressif board (expected vendor 303a)");
  }

  const module = await loadEsptool();
  const baud = Number(baudSelect.value) || 115200;
  port = selected;
  transport = new module.Transport(port, false);
  loader = new module.ESPLoader({ transport, baudrate: baud, terminal: terminal() });
  log("connecting at " + baud + " baud…");
  const chip = await loader.main();
  chipEl.textContent = chip;
  log("connected: " + chip);
  if (!/ESP32-S3/i.test(chip)) {
    throw new Error("this image is for the ESP32-S3; the connected chip is " + chip);
  }
  setStatus("Connected. Starting the write.", "meta-ok");
  disconnectBtn.disabled = false;
  return chip;
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
}

async function writeImage() {
  if (!loader || !bytes) throw new Error("nothing to write");
  progressBar.style.width = "0%";
  progressPct.textContent = "0%";
  progressNote.textContent = "writing at 0x0";
  setStatus("Flashing. Do not unplug the cable.", "");
  await loader.writeFlash({
    fileArray: [{ data: bytes, address: 0x0 }],
    flashMode: "keep",
    flashFreq: "keep",
    flashSize: "keep",
    eraseAll: eraseBox.checked,
    compress: true,
    reportProgress: (index, written, total) => {
      const pct = total > 0 ? Math.floor((written / total) * 100) : 0;
      progressBar.style.width = pct + "%";
      progressPct.textContent = pct + "%";
      progressNote.textContent = humanSize(written) + " of " + humanSize(total);
    },
  });
  progressBar.style.width = "100%";
  progressPct.textContent = "100%";
  progressNote.textContent = "done";
  log("flash complete");
  setStatus("Flashed. Resetting the board.", "meta-ok");
  try {
    await loader.after("hard_reset");
  } catch (error) {
    log("reset: " + (error.message || error));
  }
  await teardown();
  setStatus("Done — the board is booting " + release.name + ".", "meta-ok");
}

async function install() {
  if (busy || !release) return;
  busy = true;
  installBtn.disabled = true;
  connectBtn.disabled = true;
  refreshBtn.disabled = true;
  try {
    if (!bytes) await downloadAndVerify();
    if (!loader) await connect();
    await writeImage();
  } catch (error) {
    log("install failed: " + (error.message || error));
    setStatus(String(error.message || error), "meta-bad");
    progressNote.textContent = "stopped";
    await teardown();
  } finally {
    busy = false;
    connectBtn.disabled = false;
    refreshBtn.disabled = false;
    updateInstallButton();
  }
}

async function connectOnly() {
  try {
    await connect();
  } catch (error) {
    log("connect failed: " + (error.message || error));
    setStatus(String(error.message || error), "meta-bad");
    await teardown();
  }
}

async function disconnectOnly() {
  log("disconnected");
  setStatus("Disconnected.");
  await teardown();
}

el("file").addEventListener("change", async (event) => {
  const file = event.target.files && event.target.files[0];
  if (!file) return;
  try {
    const buffer = new Uint8Array(await file.arrayBuffer());
    const digest = await sha256(buffer);
    shaEl.textContent = digest;
    if (!expectedSha) throw new Error("no published hash to compare against; press Check for updates first");
    if (digest !== expectedSha) {
      bytes = null;
      shaEl.className = "meta-bad";
      throw new Error("that file is not " + release.name + " (hash does not match)");
    }
    bytes = buffer;
    shaEl.className = "meta-ok";
    setStatus("Using " + file.name + " — hash verified.", "meta-ok");
    log("using local file " + file.name + " (" + humanSize(buffer.length) + ")");
  } catch (error) {
    bytes = null;
    log("file refused: " + (error.message || error));
    setStatus("File refused: " + (error.message || error), "meta-bad");
  }
});

installBtn.addEventListener("click", install);
refreshBtn.addEventListener("click", loadRelease);
connectBtn.addEventListener("click", connectOnly);
disconnectBtn.addEventListener("click", disconnectOnly);

if (!("serial" in navigator)) {
  setStatus("Web Serial is unavailable. Open this page in desktop Chrome or Edge 89+ over https or localhost.", "meta-bad");
}
log("Ready. Install downloads the latest release, checks its SHA-256, writes it at 0x0 and resets the board.");
loadRelease();
