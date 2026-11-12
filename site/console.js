/* Live serial console over Web Serial. Read-only with respect to the board
   except for the lines the user types. */
const logEl = document.getElementById("console-log");
const inputEl = document.getElementById("cmd");
const connectBtn = document.getElementById("connect-btn");
const disconnectBtn = document.getElementById("disconnect-btn");
const clearBtn = document.getElementById("clear-btn");
const copyBtn = document.getElementById("copy-btn");
const saveBtn = document.getElementById("save-btn");
const statusEl = document.getElementById("serial-status");
const dotEl = document.getElementById("serial-dot");
const baudEl = document.getElementById("baud");

const MAX_CHARS = 200000;

let port = null;
let reader = null;
let writer = null;
let reading = false;
let decoder = new TextDecoder();
let encoder = new TextEncoder();

function append(text) {
  if (!logEl) return;
  const atBottom = logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 40;
  logEl.textContent += text;
  if (logEl.textContent.length > MAX_CHARS) {
    logEl.textContent = logEl.textContent.slice(-MAX_CHARS);
  }
  if (atBottom) logEl.scrollTop = logEl.scrollHeight;
}

function setStatus(text, live) {
  statusEl.textContent = text;
  dotEl.className = "dot" + (live ? " on" : "");
}

function supported() {
  return "serial" in navigator;
}

async function readLoop() {
  reading = true;
  while (reading && port && port.readable) {
    reader = port.readable.getReader();
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) append(decoder.decode(value, { stream: true }));
      }
    } catch (error) {
      if (reading) append("\n[serial: " + (error.message || error) + "]\n");
    } finally {
      try {
        reader.releaseLock();
      } catch (error) {
        /* already released */
      }
      reader = null;
    }
  }
}

async function connect() {
  if (!supported()) {
    setStatus("Web Serial is unavailable. Use Chrome or Edge 89+ on desktop.", false);
    return;
  }
  connectBtn.disabled = true;
  setStatus("Waiting for a port…", false);
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: Number(baudEl.value) || 115200 });
    writer = port.writable.getWriter();
    decoder = new TextDecoder();
    append("\n[connected at " + baudEl.value + " baud — opening the port resets the board]\n");
    setStatus("Connected at " + baudEl.value + " baud.", true);
    disconnectBtn.disabled = false;
    readLoop();
  } catch (error) {
    append("\n[connect failed: " + (error.message || error) + "]\n");
    setStatus(String(error.message || error), false);
    await disconnect(true);
  } finally {
    connectBtn.disabled = false;
  }
}

async function disconnect(silent) {
  reading = false;
  try {
    if (reader) await reader.cancel();
  } catch (error) {
    /* port already gone */
  }
  try {
    if (writer) {
      writer.releaseLock();
      writer = null;
    }
  } catch (error) {
    writer = null;
  }
  try {
    if (port) await port.close();
  } catch (error) {
    /* may already be closed */
  }
  port = null;
  reader = null;
  disconnectBtn.disabled = true;
  setStatus("Not connected.", false);
  if (!silent) append("\n[disconnected]\n");
}

async function send(text) {
  if (!writer) {
    append("\n[not connected]\n");
    return;
  }
  try {
    await writer.write(encoder.encode(text + "\r\n"));
    append("\n> " + text + "\n");
  } catch (error) {
    append("\n[send failed: " + (error.message || error) + "]\n");
  }
}

connectBtn.addEventListener("click", connect);
disconnectBtn.addEventListener("click", () => disconnect(false));
clearBtn.addEventListener("click", () => {
  logEl.textContent = "";
});
copyBtn.addEventListener("click", async () => {
  const text = logEl.textContent || "";
  try {
    await navigator.clipboard.writeText(text);
    const label = copyBtn.textContent;
    copyBtn.textContent = "Copied";
    window.setTimeout(() => {
      copyBtn.textContent = label;
    }, 1400);
  } catch (error) {
    append("\n[copy failed: " + (error.message || error) + "]\n");
  }
});
saveBtn.addEventListener("click", () => {
  const blob = new Blob([logEl.textContent || ""], { type: "text/plain" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  link.href = url;
  link.download = "vandeta-console-" + stamp + ".log";
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  URL.revokeObjectURL(url);
});

document.getElementById("cmd-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const text = inputEl.value.trim();
  if (!text) return;
  inputEl.value = "";
  send(text);
});

document.querySelectorAll("[data-cmd]").forEach((button) => {
  button.addEventListener("click", () => {
    const command = button.getAttribute("data-cmd");
    inputEl.value = command;
    inputEl.focus();
    send(command);
  });
});

document.querySelectorAll("[data-fill]").forEach((button) => {
  button.addEventListener("click", () => {
    inputEl.value = button.getAttribute("data-fill");
    inputEl.focus();
  });
});

window.addEventListener("beforeunload", () => {
  if (port) disconnect(true);
});

if (!supported()) {
  setStatus("Web Serial is unavailable. Use Chrome or Edge 89+ on desktop over https or localhost.", false);
  connectBtn.disabled = true;
  append(
    "[this browser has no Web Serial API]\n" +
      "Open this page in desktop Chrome or Edge. On Linux the board needs the cdc_acm driver: sudo modprobe cdc_acm.\n"
  );
} else {
  setStatus("Not connected.", false);
  append("Vandeta USB console.\nTurn on Config > System Config > Developer Options on the board, then connect.\n");
}
