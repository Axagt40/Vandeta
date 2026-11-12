# Security policy

Vandeta is **offensive-security tooling**. It is built to transmit, intercept
and inject on WiFi, BLE, sub-GHz, nRF24 and infrared, and to type into a host
over USB. Running it against hardware or networks you do not own or are not
explicitly authorised to test is illegal in most jurisdictions and is not a
supported use.

## Reporting a vulnerability

Use GitHub's private reporting channel:

**<https://github.com/Axagt40/Vandeta/security/advisories/new>**

Please do **not** open a public issue for anything in this list:

- A radio path that transmits outside the band or power the UI says it uses.
- A crash, panic or boot loop reachable from a normal menu or console command.
- Memory corruption, out-of-bounds reads or writes, or a leaked buffer in a
  protocol parser (Sub-GHz, IR, BLE advertisements, nRF24 packets, RFID).
- A way to make the flasher page install an image whose SHA-256 does not match
  the published digest.
- A remote path that survives a reboot, or that runs without Developer Mode.

Include the firmware version from `info`, the build id, what you did and the
`errors` output. A reproduction on the board is worth more than a code reading,
but a code reading is still welcome.

## What is not a vulnerability

- Needing Developer Mode for the console. That is the design.
- The self-signed HTTPS endpoint in the MITM engine: it exists so a client can
  be intercepted, and it warns by design.
- Bruce upstream bugs that also reproduce on stock Bruce 1.16.1 - please report
  those upstream as well, so the fix can land in both.
- The absence of the features listed under *What this build does not do* in the
  README. They are documented limits, not defects.

## Scope and response

There is no bug bounty and no response-time guarantee. Reports are read, and a
fix ships in the next build if it is reproducible. Credit is given in the
release notes unless you ask otherwise.
