# Roadmap

## Project goal

Open-source release on GitHub for the embroidery user base — free to use.
Hard constraint: flashing the firmware and running the backend must be
**foolproof on Linux and Windows** for non-technical users.

This constraint is load-bearing for the features below: anything that
requires baking in WiFi credentials, hardcoded IPs, a dev toolchain, or a
terminal is disqualified for the public release.

## Requirements derived from the foolproof constraint

- **No hardcoded WiFi credentials.** Every user has their own network —
  credentials can no longer be flashed via NVS at build time.
- **No manual IP/host configuration.** The stick must find the backend
  itself.
- **No toolchain to flash firmware.** Use [ESP Web Tools](https://esphome.github.io/esp-web-tools/)
  (browser-based flashing over WebSerial — Chrome/Edge only, no installs).
  Requires hosting a small manifest + prebuilt `.bin` (e.g. GitHub Pages).
- **No Go install to run the backend.** Ship prebuilt cross-compiled
  executables (Windows `.exe` + Linux binary) per GitHub release, built via
  CI. User double-clicks; no CLI required for the common case.
- **License.** Pick an OSS license before the first public release (MIT is
  the likely default for a hobby project like this).
- **CI/release pipeline.** GitHub Actions: on tag push, build firmware
  `.bin` + backend binaries for Windows/Linux and attach to the GitHub
  release.

## Features

1. **WiFi AP provisioning mode** — implemented (`main/app_provision.c`,
   `main/app_button.c`, `main/app_led.c`).
   - Confirmed hardware (official M5Stack docs): button = GPIO41
     (active-low), onboard WS2812 RGB LED = GPIO35.
   - No stored SSID, or button held 3s at boot, or STA connect times out
     (20s default) → device starts an open SoftAP
     (`EmbroideryStick-Setup`) + captive portal (scanned SSID dropdown +
     password field, `esp_http_server` + a hand-rolled DNS wildcard
     responder for auto-popup). Submitting saves credentials via
     `app_wifi_save_credentials()` and reboots into STA mode.
   - Status LED: blue blink = provisioning, amber blink = connecting,
     cyan blink = WiFi up but backend not confirmed yet, solid green =
     WiFi + backend both confirmed, solid red = connect error (brief,
     before falling back to provisioning).
   - USB MSC only comes up after WiFi is confirmed connected — no drive
     is presented during provisioning (deliberate; see plan notes).
   - Rejected alternative: ESP-IDF's official `wifi_provisioning`
     component (BLE/protocomm) — built for pairing with a companion app,
     too much moving complexity for a single-user device with no app.
   - Remaining: physical verification (button hold, LED colors, real
     phone captive-portal auto-popup on iOS/Android) — see plan at
     implementation time for the full verification checklist.

2. **Backend auto-discovery** — implemented (`main/app_discovery.c`,
   `backend/main.go`'s `runDiscoveryResponder`).
   - ESP broadcasts a fixed magic string (`EMBROIDERY_DISCOVER_V1`) to
     `255.255.255.255` on UDP port 7891 (separate from the TCP control
     port 7892); the backend's UDP listener replies with its TCP port —
     the IP is taken from the reply packet's source address, not sent on
     the wire. Retries a few times with a short per-attempt timeout.
   - Manual override still works: if NVS key `backhost` is set
     (`EMBROIDERY_BACKEND_HOST` Kconfig default is now `""`, was a
     personal dev IP), discovery is skipped entirely. Holding the
     provisioning button also clears `backhost`/`backport` from NVS, so
     "start fresh" resets backend pinning, not just WiFi.
   - `version_poll_task` re-attempts discovery whenever the backend is
     unreachable (and no manual override is set) and switches to a newly
     found host automatically — covers "stick powered on before the
     backend laptop was even running" without a manual reboot.
   - Rejected alternative: mDNS — would require avahi-daemon on the Linux
     host plus the ESP-IDF `mdns` component, and multicast is flaky on
     some routers. Custom UDP broadcast keeps the project dependency-free
     (matches the existing stdlib-only Go backend) and is simpler to
     reason about for a single LAN.
   - Verified on real hardware: confirmed discovery (not a stale manual
     override) actually resolves the backend by moving the backend to a
     different port and watching the LED recover from cyan to green.

## Nice to have (not needed for initial release)

3. **Multiple embroidery machines, single shared backend**
   - Each machine gets its own embroidery stick; all sticks talk to one
     central backend serving one file directory.
   - Mostly already supported: `handleConn` in `backend/main.go` already
     runs per-connection in its own goroutine with its own `sessionFiles`
     snapshot, and `catalog` access is guarded by a `sync.RWMutex` — so
     concurrent sticks from different machines can already connect to the
     same backend without changes.
   - Remaining open question (not urgent): whether concurrent `READ_FILE`
     fetches from multiple sticks need any throttling/fairness on the
     backend side once there's real concurrent load — not a concern at
     current usage levels.

## Notes

- Local git history for this project lives in this directory's own `.git`
  (independent from the upstream `esp-iot-solution` repo it currently sits
  inside of — see project memory for why).
- Backend currently only watches a single flat directory (no
  subdirectories) and polls for changes every 2s (see `backend/main.go`).
- 8.3-truncated filenames that collide (common once there are many files
  sharing the same first 8 characters) now get a Windows-style `~N`
  disambiguating suffix instead of silently looking identical on the
  embroidery machine — reads always worked correctly either way (each
  file keeps its own `file_id`/cluster chain), this was purely a display
  ambiguity fix.
- Allowed file types (`.PES`/`.DST`/`.JEF`/...) are no longer hardcoded —
  `backend/extensions.conf` (auto-created next to the executable on first
  run, gitignored) lists them, one per line, `#` to disable. Default is
  PES-only with the others commented out, since the served directory
  often also contains non-machine-readable files (images, `.EMB` editing
  files, etc.) that would otherwise clutter the small virtual disk.
- `do_usb_refresh()`'s VBUS detach delay is 500ms (was 50ms) — some host
  USB stacks/file managers debounced the shorter detach as a glitch and
  never noticed the disk content had changed.
