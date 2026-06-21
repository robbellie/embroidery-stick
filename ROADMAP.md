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

## Features (in planning, not yet implemented)

1. **WiFi AP provisioning mode**
   - Hold button at boot → device starts a SoftAP + captive portal
     (HTML form: scan list of SSIDs + password field).
   - Credentials saved to NVS, device reboots into STA (client) mode.
   - Onboard RGB LED indicates mode (e.g. blinking = provisioning, solid =
     connected) — exact pattern TBD.
   - Rejected alternative: ESP-IDF's official `wifi_provisioning`
     component (BLE/protocomm) — built for pairing with a companion app,
     too much moving complexity for a single-user device with no app.
   - Open item: confirm exact GPIO pins for button + RGB LED on the
     AtomS3U (not yet verified against hardware docs).

2. **Backend auto-discovery**
   - ESP broadcasts a small custom UDP "DISCOVER" packet on the LAN; the
     backend listens and replies with its IP + port.
   - Rejected alternative: mDNS — would require avahi-daemon on the Linux
     host plus the ESP-IDF `mdns` component, and multicast is flaky on
     some routers. Custom UDP broadcast keeps the project dependency-free
     (matches the existing stdlib-only Go backend) and is simpler to
     reason about for a single LAN.

## Notes

- Local git history for this project lives in this directory's own `.git`
  (independent from the upstream `esp-iot-solution` repo it currently sits
  inside of — see project memory for why).
- Backend currently only watches a single flat directory (no
  subdirectories) and polls for changes every 2s (see `backend/main.go`).
