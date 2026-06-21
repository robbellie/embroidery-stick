# Embroidery Stick

A USB stick for embroidery machines that pretends to be a regular flash
drive, but actually streams files wirelessly from a folder on your
computer — no copying files back and forth, no running out of space.

Plug the stick into your embroidery machine. It connects to your WiFi,
finds the backend program running on your computer, and presents your
embroidery files (`.PES`/`.DST`/`.JEF`/...) as if they were sitting right
there on the stick. Add or remove a file in the folder on your computer
and it shows up on the machine within seconds — no need to unplug
anything.

## Hobby project — basic support, please be patient

This is shared as-is, for free, because it might be useful to someone
else. It's not a product — I work a full-time job plus a side job, so
I'll try to help with issues when I can, but response times will vary
and I can't promise a fix on any particular timeline. Pull requests and
forks are very welcome.

Tested specifically on the **M5Stack AtomS3U**. It will very likely work
on other ESP32(-S3) boards with native USB (button/LED GPIOs are
configurable in `menuconfig`), but there are almost certainly
better-suited devices out there — boards with more RAM in particular
would allow more/larger cache slots and faster, smoother file serving
than this one. If you want to run this on different hardware, you're
welcome to dig in and try it yourself — and if it doesn't work and you'd
like my help debugging it, the most reliable way is to get me a unit of
that hardware to test with directly.

## What you need

- An ESP32-S3 board with native USB — this project targets the
  [M5Stack AtomS3U](https://docs.m5stack.com/en/core/AtomS3U) (button on
  GPIO41, RGB LED on GPIO35; both configurable in `menuconfig` if you use
  a different board).
- A computer on the same WiFi network/LAN as the embroidery machine, to
  run the backend program on.

## Quick start

### 1. Flash the firmware

*(Browser-based flashing via [ESP Web Tools](https://esphome.github.io/esp-web-tools/) is planned — for now, build from source, see below.)*

### 2. First-time WiFi setup

On first boot (or anytime you hold the physical button for 3 seconds at
power-on), the stick creates its own WiFi network called
**`EmbroideryStick-Setup`** and the status LED blinks blue.

1. Connect your phone or laptop to that network.
2. A setup page should open automatically. If not, open
   `http://192.168.4.1` in a browser.
3. Pick your WiFi network from the list and enter the password.
4. Submit — the stick reboots and joins your network.

If it can't connect (e.g. wrong password), it automatically goes back
into setup mode after about 20 seconds — no need to find a reset button.

Holding the button again later also clears any manually-pinned backend
address, so "start fresh" resets everything back to automatic.

### 3. Run the backend

The backend is a small program that serves your embroidery files over
the network. Download the binary for your OS from
[Releases](../../releases) *(coming soon)*, or build it yourself:

```sh
cd backend
go build -o embroidery-backend .
./embroidery-backend -dir /path/to/your/embroidery/files
```

That's it — the stick finds it automatically (UDP broadcast discovery,
no IP address to type anywhere). On first run, the backend creates an
`extensions.conf` file next to itself listing which file types it
serves (only `.PES` by default — edit the file to enable `.DST`, `.JEF`,
etc.).

## Status LED

| Color / pattern | Meaning |
|---|---|
| Blue, blinking | Setup mode — connect to `EmbroideryStick-Setup` |
| Amber, blinking | Connecting to WiFi |
| Cyan, blinking | WiFi connected, looking for the backend |
| Green, solid | Fully connected — everything's working |
| Red, solid | WiFi connection failed (briefly, before retrying setup mode) |

## Performance tips

The stick is built for small files — embroidery files typically are
(tens of KB), and the cache/RAM budget on a small ESP32-S3 board is
limited.

Keep the served folder relatively small (don't point it at an entire
archive of hundreds of files). Embroidery machines commonly generate a
thumbnail preview for every file they see in a folder, and on a stick
like this that means fetching each one over WiFi — the more files
visible at once, the slower browsing feels on the machine. Move files
in and out of the served folder as you work, rather than dumping
everything in at once. (The backend doesn't currently look into
subfolders — see `ROADMAP.md`.)

## Advanced

- **Manual backend address**: if auto-discovery doesn't work on your
  network (e.g. broadcast traffic is blocked), set a fixed address via
  `idf.py menuconfig` → "Embroidery Stick Configuration" → "Backend
  hostname or IP", or at runtime via NVS. Holding the button clears this.
- **Multiple embroidery machines**: one backend can serve multiple
  sticks at once — nothing extra to configure.
- **File-watching**: the backend polls the served folder every 2
  seconds and also reloads on `SIGHUP` (`kill -HUP <pid>`).

## Building from source

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (v5.0+)
for the firmware and [Go](https://go.dev/) (1.21+) for the backend.

```sh
# Firmware
idf.py build
idf.py -p /dev/ttyACM0 flash

# Backend
cd backend && go build -o embroidery-backend .
```

See `ROADMAP.md` for the project's design notes and what's still planned.

## License

MIT — see `LICENSE`.
