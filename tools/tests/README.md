# Device tests

Host-side scripts that drive a flashed Vandeta board over its USB cable. Every
check is either a PASS or a FAIL: a missing fixture, an unset option or an
unplugged cable fails the run instead of quietly skipping a step.

## What they need

| Requirement | Why |
|---|---|
| The board on USB, `303a:1001` visible | The scripts talk over the console device (`/dev/ttyACM0` by default) |
| `Config > System Config > Developer Options` on | `ui`, `tune`, `patch`, `file`, `mem` and the rest are gated behind it. A flash at `0x0` clears NVS, so turn it on again after one |
| No other program holding the port | The screen mirror and `pio device monitor` take the port; close them first |
| `pyserial` | `pip install pyserial` |
| The `sd_files/` payloads on the card | Copy the folder to the card root: `/BruceSpoof`, `/BruceJS`, `/infrared` and friends are what the MITM, JavaScript and IR checks load |

The scripts reset the board first, so start from a normal boot: unplug anything
that would keep a page open and let the board reach the main menu.

## Running one

```sh
python3 tools/tests/t26_mitm_menus.py            # default port /dev/ttyACM0
python3 tools/tests/t26_mitm_menus.py /dev/ttyACM1
```

Each script prints one line per check, then a summary:

```
===== SUMMARY =====
  36 passed, 0 failed, 0 skipped
```

The exit status is non-zero when anything failed, so a loop over the directory
can gate on it:

```sh
for t in tools/tests/t*.py; do python3 "$t" || echo "FAILED: $t"; done
```

## Coverage

| Script | What it checks |
|---|---|
| `t17_*` | RF capture paths and the WiFi source for the packet monitor |
| `t18_packet_monitor_radios.py` | Packet monitor fed by each radio in turn |
| `t19_ir_universal_bench.py` | Universal Remote code tables |
| `t20_usb_verify_task1.py` | Developer Options, the console verbs and the binary protocol |
| `t22_wardriver_identify.py` | Wardriver capture identify and the decoded copy |
| `t23_wardriver_regressions.py` | Wardriver identify/replay limits, oversized captures, the Mode rows |
| `t24_dev_mode_toggle.py` | Developer Mode on/off gate |
| `t25_v11_new_features.py` | The dev 1.1 scripting APIs |
| `t26_mitm_menus.py` | MITM Attack page, `.mitm` payloads, the `mitm` / `dnsinject` / `htmlint` verbs |
| `t27_mitm_live.py` | Live interception against a phone (needs a second device) |
| `t28_task1_ui_checks.py` | Launcher grid, status strip, menu navigation |
| `t29_task1_settings.py` | Settings pages and the persisted values |
| `t30_badir_family_test.py` | Infrared family/brand sweep |
| `t31_mirror_protocols.py` | The screen mirror protocol |
| `t32_half2_checks.py`, `t33_vandeta_suite.py` | Menu and binding regression sweeps |
| `t37_vandeta_13_device.py` | The 1.3 feature set on device |
| `t38_usb_eth_bridge.py`, `t39_eth_bridge_host.py` | The USB Ethernet bridge |

Companion payloads (`.js`, `.txt`, `.lay`) stay in `FREEBUFF/tests/`, where the
development notes that use them live.
