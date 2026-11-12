# Console reference

The USB console is the reason this build exists: it drives and inspects the
device from a PC over the same USB cable, with no WiFi and no extra hardware.
This file is the verb contract; `site/console.html` is the same connection in a
browser tab.

## Turning it on

**Config → System Config → Developer Options → Developer Mode.** One switch arms
the console and the speed profile together. With it off, every dev-gated verb
answers that Developer Mode is off and the resources are released.

Connect at **115200**, 8N1. The console boots in plain text and only switches
modes when asked, so a mistyped command cannot leave the board unusable.

> **Opening the serial port resets the board.** That is the USB-Serial-JTAG
> bridge, not a firmware fault - every session starts with one boot.

## Menus and screen

| Command | What it does |
|---------|--------------|
| `ui` | Prints the menu on screen: title, every row, the highlighted row. |
| `ui down`, `ui up`, `ui select` | Moves the highlight or opens the row, then prints the screen you land on. |
| `ui watch on\|off` | Streams every screen change as it happens. |
| `move up\|down\|left\|right\|nextpage` | Key injection with the same readback. |
| `press ok\|back` | Presses the encoder or Back and reports the new screen. |
| `display ring on\|off`, `display dump` | Inspects what a screen actually drew. |

## Files, logs and system

| Command | What it does |
|---------|--------------|
| `file read\|write\|list\|delete\|stat` | File operations relative to the console's working directory. |
| `log show\|clear\|on\|off` | The serial log ring. |
| `errors` | The error ring: ESP-IDF, Arduino `log_x()` and plain `printf()` / NimBLE lines together. |
| `mem`, `proc`, `sys`, `info` | Free memory, task list, system information, firmware version. |
| `md5`, `crc32`, `cat`, `storage read` | Streamed in 512-byte chunks, so any file size works. |
| `help <command>` | Usage for one command; bare `help` lists them. |

## Developer features

| Command | What it does |
|---------|--------------|
| `usbdebug status\|proto\|on\|off` | Console state, and the framed binary protocol for reliable transfers. |
| `livepatch apply <module> <code>` | Reloads the JavaScript layer without a reflash. |
| `liveboot enable\|disable\|status` | Runs a script at boot through the Startup App mechanism. |
| `js run <file>`, `js stop` | Starts or stops the interpreter headlessly. |
| `tune list\|get\|set\|reset\|save\|load` | The 15 persisted, range-checked tuning values (see the Tunables page). |
| `patch list`, `patch <name> <index>` | Selects a variant table (`rf_scan`, `ir_repeat`, `nrf_capture`). |
| `devflash` | Pushes an app-only image over USB. |

## Radios, IR and BadUSB

| Command | What it does |
|---------|--------------|
| `subghz rx <seconds>` | Listens for a timed window on the CC1101. |
| `subghz wardriver capture\|list\|identify\|identifyall\|replay\|status\|selftest` | Records, browses, decodes and replays wardriver captures headlessly. |
| `nrfcap` / `nrf` `info\|init\|promisc\|channel\|rate\|power\|freqrange\|scan\|capture\|capch\|lock\|rgb\|multifreq\|profile\|profiles\|repeat\|list\|replay\|save\|load\|analyze` | The nRF24 side: pseudo-promiscuous capture, channel runs, profiles, replay, packet analysis. |
| `rfbits` | The sub-GHz bit editor behind `RF > RF Suite > Bit Editor`. |
| `ir uni list\|buttons\|send` | Universal Remote: categories, button names, a code sweep. |
| `ir ac list\|send` | The twelve brand AC encoders. |
| `ir rx <seconds>` | Records an IR frame for a timed window. |
| `irnorm` | Learns, normalises and saves one capture as a `.ir` remote. |
| `badusb config\|set\|layout-import\|layout-export\|layout-copy\|mouse\|click` | USB footprint, layout handling, HID device type, pointer input. |
| `modem send <text> [baud] [ultrasonic]` | The FSK audio modem. `modem recv` reports that this board has no microphone. |

## WiFi and interception

| Command | What it does |
|---------|--------------|
| `moreatk` | Runs the WiFi attacks headlessly, with a deadline. |
| `mitm status\|payload\|target\|run\|stop` | The MITM Attack page without the keypad. |
| `dnsinject status\|payload\|rule\|wildcard\|rules\|clear\|example\|scan\|arp\|arpstop\|start\|stop\|captures` | DNS injection alone. |
| `htmlint status\|match\|payload\|redirect\|rules\|clear\|example\|scan\|arp\|arpstop\|start\|stop\|captures` | HTML interception without the keypad. |

## Conventions

- Commands are matched as whole words: `patch/list` never routes.
- A negative number must be quoted: `tune set rf.noise_floor "-95"`.
- `subghz scan` takes **Hz** and divides by 1e6 itself.
- `settings <name>` with no value reads; it never writes a default.
- `settings` without an argument dumps the whole config as JSON.
