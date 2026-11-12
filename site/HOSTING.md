# Hosting this site

The site is plain static files: `site/`. GitHub Pages publishes it through
`.github/workflows/pages.yml`, and nothing is generated, so any static host will
serve it the same way.

The one constraint that decides where it can be hosted: browsers only expose
**Web Serial** - which the flasher needs - on a **secure origin**. That means
`https://` or `http://localhost`. A plain `http://192.168.x.x` page loads fine
but its *Connect* button will not work.

| Where | How | Flasher works? |
|-------|-----|----------------|
| GitHub Pages | Public repository, `.github/workflows/pages.yml` publishes `site/` on every push to `main`. Served at `https://axagt40.github.io/Vandeta/`. | Yes |
| This computer | `python3 -m http.server 8000 --directory site`, then open `http://localhost:8000`. No account, no upload. | Yes - `localhost` is a secure origin |
| Phone (Termux / proot) | `python3 -m http.server 8000 --directory site` in Termux or Debian; other devices open `http://<phone-ip>:8000`. For HTTPS put a free tunnel (for example a cloudflared quick tunnel) in front of it. | Only through the tunnel - plain LAN HTTP is not secure |

**The board plugs into the computer, not the phone.** The page may be served from
anywhere, but `navigator.serial` only sees devices attached to the machine
running Chrome. Serving this folder from a phone is fine for reading the docs;
flashing wants the board on the same computer as the browser.

**Internet, but no account.** The flasher reads the release list from the GitHub
API, pulls the image from the repository and imports the pinned `esptool-js`
0.7.0 bundle from unpkg. Its channel buttons follow GitHub's own flags:
*Official* is the newest release that is not marked pre-release, *Pre-release*
the newest one that is. With no connection it falls back to the bundled copy in
`site/firmware/` - Vandeta 1.3, published 2026-11-02, sha256
`230cd94b...f4ba8a` - labelled as an offline copy. A pre-release is never
offered offline.

The flasher always installs the newest release of the selected channel, so
cutting a release needs no site edit. Keep `site/firmware/vandeta.bin` with its
`.sha256` current anyway: that is what an offline visitor gets, and the page
still refuses any image whose hash does not match the release digest.
