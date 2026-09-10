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
   `backend/embroidery/server.go`'s `discoveryLoop`).
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

3. **Backend subfolder support + cross-platform GUI** — implemented
   (protocol v2 in `main/embroidery_protocol.h`, `backend/embroidery/`,
   `main/app_virtual_fat.c`, `backend/guiapp/`).
   - Real nested subdirectories, not flattened/prefixed names: the wire
     format (`proto_file_info_t`) carries `id`/`parent_id`/`is_dir` for
     every entry (files and directories share one flat, parent-pointer
     list), and `app_virtual_fat.c` builds an actual per-directory FAT
     entry table (with synthesized `.`/`..`) so folders show up as real,
     browsable folders on the embroidery machine.
   - `backend/embroidery/` is now an importable Go package
     (`filepath.WalkDir`-based catalog, per-directory 8.3 disambiguation,
     `Start`/`Stop`/`Reload` lifecycle) shared by both the headless CLI
     (`backend/cmd/embroidery-backend`) and the new desktop GUI
     (`backend/cmd/embroidery-backend-gui`, built with Fyne) — folder
     picker, port field, Start/Stop, live status, and a flash indicator
     on file reads.
   - Rejected alternative for the GUI toolkit: none seriously considered
     besides Fyne — it compiles to one self-contained binary per OS with
     no separate runtime to install, matching the "foolproof" constraint.

4. **Second board: Waveshare ESP32-S3-GEEK** — implemented (`main/app_sd_cache.c`,
   `main/app_status_lcd.c`, `sdkconfig.defaults.geek`).
   - Adds an SD card write-through cache (files stay available if the
     backend goes offline — see `APP_LED_STATE_OFFLINE`) and an on-device
     status LCD in place of the AtomS3U's single WS2812 LED. Board choice
     is a Kconfig option (`EMBROIDERY_BOARD`); GPIOs, the status backend,
     and SD-cache availability are all conditioned on it, so one source
     tree builds either firmware.
   - **Root-caused a severe, hard-to-diagnose bug** during bring-up: with
     the SD cache enabled, the USB drive would go permanently
     unresponsive (every sector read stuck at "pending" forever, host
     eventually resets the device) once the served catalog grew past a
     certain size — but *only* with the SD cache active; the same catalog
     worked fine with it disabled. Ten different scheduling/timing fixes
     (task priority, CPU core affinity, pacing eager-fill activity,
     holding VBUS off until a sync pass finished, deferring the SD mount
     until after USB was already up) all failed to change the symptom at
     all, including a case where the sync pass finished near-instantly
     (nothing new to fetch) and the bug *still* reproduced — proof the
     issue had nothing to do with the sync task's actual activity.
   - Actual cause: `CONFIG_SPIRAM` had never been enabled for this board
     (no per-board Kconfig default exists for it, unlike the other
     board-specific settings). Every `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
     call in `app_virtual_fat.c` was silently falling back to scarce
     internal RAM; combined with the SD cache's own ~32KB static
     allocation, this exhausted available memory once the catalog needed
     enough clusters, making `vfat_init()` fail allocation — and
     `do_usb_refresh()` never checked its return value, so it reattached
     USB anyway with the virtual disk stuck uninitialized.
   - Found via a temporary UDP-broadcast log sink (`esp_log_set_vprintf`
     redirecting every log line to a LAN broadcast, since the USB-serial
     console isn't available once the app is running in USB-MSC mode) —
     it caught an explicit `E vfat: Allocation failed` right before the
     stall began. Fixed by enabling `CONFIG_SPIRAM` (Quad mode, 2MB —
     this board's chip is an ESP32-S3R2) in `sdkconfig.defaults.geek`,
     applied via `-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.geek"`
     so it can never again regress from a manual `sdkconfig` edit missing
     it.
   - General lesson worth remembering: hand-editing an existing
     `sdkconfig`'s board choice instead of regenerating from
     `sdkconfig.defaults` leaves other board-dependent settings stuck at
     their old values, since Kconfig `default X if Y` only applies when a
     symbol isn't already set in the file.

## Nice to have (not needed for initial release)

5. **Multiple embroidery machines, single shared backend**
   - Each machine gets its own embroidery stick; all sticks talk to one
     central backend serving one file directory.
   - Mostly already supported: `handleConn` in
     `backend/embroidery/server.go` already runs per-connection in its
     own goroutine with its own `sessionNodes` snapshot, and `Catalog`
     access is guarded by a `sync.RWMutex` — so
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
- Backend watches the served directory tree recursively (subdirectories
  included) and polls for changes every 2s (see `backend/embroidery/watch.go`).
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
  files, etc.) that would otherwise clutter the small virtual disk. The
  GUI's "File types..." button edits this same file via checkboxes
  (`embroidery.LoadAllowedExtensions`/`SaveAllowedExtensions`) — no need
  to hand-edit it — and applies changes live via
  `Server.SetAllowedExtensions` without restarting the server.
- `do_usb_refresh()`'s VBUS detach delay is 500ms (was 50ms) — some host
  USB stacks/file managers debounced the shorter detach as a glitch and
  never noticed the disk content had changed.
