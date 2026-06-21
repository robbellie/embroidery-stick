package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"
)

/*
 * Protocol (little-endian, all frames):
 *   [cmd:1][payload_len:4][payload:N]
 *
 * CMD_HELLO    (0x01)  req: [proto_version:2]
 *                      res: [proto_version:2][disk_version:8]
 * CMD_VERSION  (0x02)  req: no payload
 *                      res: [disk_version:8]
 * CMD_LIST_FILES (0x03) req: no payload
 *                      res: [count:2][{name[13], size:4, mtime:4, file_id:2} × count]
 * CMD_READ_FILE (0x04) req: [file_id:2][offset:4][length:4]
 *                      res: [actual:4][data × actual]
 */

const (
	protoVersion = uint16(1)
	defaultPort  = 7892

	cmdHello     = byte(0x01)
	cmdVersion   = byte(0x02)
	cmdListFiles = byte(0x03)
	cmdReadFile  = byte(0x04)

	// Backend auto-discovery: one-shot UDP broadcast/reply, separate from
	// the TCP control connection above. These must match the firmware's
	// EMBROIDERY_DISCOVERY_MAGIC/EMBROIDERY_DISCOVERY_DEFAULT_PORT in
	// embroidery_protocol.h — duplicated as matching literals since Go
	// can't include that C header (same pattern as defaultPort above).
	discoveryMagic = "EMBROIDERY_DISCOVER_V1"
	discoveryPort  = 7891
)

var le = binary.LittleEndian

// fileEntry mirrors proto_file_info_t (packed, 23 bytes on wire).
type fileEntry struct {
	name   [13]byte
	size   uint32
	mtime  uint32
	fileID uint16
	path   string // local filesystem path (not sent on wire)
}

// catalog holds the current set of embroidery files.
type catalog struct {
	mu          sync.RWMutex
	dir         string
	allowedExt  map[string]bool // e.g. ".PES" -> true
	files       []fileEntry
	diskVersion uint64
}

func newCatalog(dir string, allowedExt map[string]bool) (*catalog, error) {
	c := &catalog{dir: dir, allowedExt: allowedExt}
	return c, c.reload()
}

// defaultExtensionsConfig is written next to the executable on first run
// if no config file exists yet, so the allowed file types are
// self-documenting and easy to extend without reading source code.
const defaultExtensionsConfig = `# Embroidery Stick - allowed file types
# One extension per line (leading dot optional). Lines starting with #
# are ignored. Uncomment (or add) the formats your embroidery machine
# supports — only PES is served by default.

.PES
# .DST
# .JEF
# .EXP
# .VP3
# .XXX
# .HUS
`

// loadAllowedExtensions reads a simple newline-delimited extension list
// from path, creating it with defaultExtensionsConfig if it doesn't exist
// yet. Deliberately not a TOML/YAML/JSON config — stdlib-only, matches
// the rest of this backend.
func loadAllowedExtensions(path string) (map[string]bool, error) {
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		if werr := os.WriteFile(path, []byte(defaultExtensionsConfig), 0o644); werr != nil {
			log.Printf("could not write default %s: %v", path, werr)
		} else {
			log.Printf("wrote default file-type config to %s", path)
		}
		data = []byte(defaultExtensionsConfig)
	} else if err != nil {
		return nil, err
	}

	allowed := make(map[string]bool)
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if !strings.HasPrefix(line, ".") {
			line = "." + line
		}
		allowed[strings.ToUpper(line)] = true
	}
	return allowed, nil
}

// reload reads the directory and updates files + diskVersion if anything changed.
func (c *catalog) reload() error {
	entries, err := os.ReadDir(c.dir)
	if err != nil {
		return fmt.Errorf("readdir %s: %w", c.dir, err)
	}

	// rawEntry holds the truncated-but-not-yet-disambiguated 8.3 name.
	// The wire protocol's name field (proto_file_info_t.name[13]) is hard
	// limited to classic 8.3 — the firmware's virtual FAT driver only
	// builds short directory entries, no VFAT/LFN long names — so two
	// files that truncate to the same 8 characters need a disambiguating
	// suffix or they'd be visually indistinguishable on the embroidery
	// machine (reads still work either way, since each gets its own
	// file_id and cluster chain, but identical-looking names are
	// confusing for the user).
	type rawEntry struct {
		base string
		ext  string
		fi   os.FileInfo
		path string
	}

	var raw []rawEntry
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		ext := strings.ToUpper(filepath.Ext(e.Name()))
		if !c.allowedExt[ext] {
			continue
		}
		fi, err := e.Info()
		if err != nil {
			continue
		}
		base := strings.ToUpper(strings.TrimSuffix(e.Name(), filepath.Ext(e.Name())))
		if len(base) > 8 {
			base = base[:8]
		}
		raw = append(raw, rawEntry{
			base: base,
			ext:  strings.TrimPrefix(ext, "."),
			fi:   fi,
			path: filepath.Join(c.dir, e.Name()),
		})
	}

	// Disambiguate truncated names that collide, Windows-short-name style:
	// first occurrence keeps its plain truncated name, later collisions
	// get a "~N" suffix carved out of the tail (e.g. TAS_NATU.PES,
	// TAS_NA~1.PES, TAS_NA~2.PES, ...).
	seen := make(map[string]int)
	var files []fileEntry
	for _, r := range raw {
		key := r.base + "." + r.ext
		seen[key]++
		name := r.base
		if n := seen[key]; n > 1 {
			suffix := fmt.Sprintf("~%d", n-1)
			cut := len(name) - len(suffix)
			if cut < 0 {
				cut = 0
			}
			name = name[:cut] + suffix
		}
		fname := name + "." + r.ext

		var nameArr [13]byte
		copy(nameArr[:], fname)

		files = append(files, fileEntry{
			name:  nameArr,
			size:  uint32(r.fi.Size()),
			mtime: uint32(r.fi.ModTime().Unix()),
			path:  r.path,
		})
	}

	sort.Slice(files, func(i, j int) bool {
		return string(files[i].name[:]) < string(files[j].name[:])
	})
	for i := range files {
		files[i].fileID = uint16(i)
	}

	c.mu.Lock()
	c.files = files
	c.diskVersion = uint64(time.Now().UnixNano())
	c.mu.Unlock()

	log.Printf("catalog reloaded: %d files, diskVersion=%d", len(files), c.diskVersion)
	return nil
}

func (c *catalog) getState() (files []fileEntry, diskVersion uint64) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	out := make([]fileEntry, len(c.files))
	copy(out, c.files)
	return out, c.diskVersion
}

func (c *catalog) getDiskVersion() uint64 {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.diskVersion
}

// ---- wire helpers ---------------------------------------------------------

func readFrame(conn net.Conn) (cmd byte, payload []byte, err error) {
	hdr := make([]byte, 5)
	if _, err = io.ReadFull(conn, hdr); err != nil {
		return
	}
	cmd = hdr[0]
	plen := le.Uint32(hdr[1:5])
	if plen > 0 {
		payload = make([]byte, plen)
		_, err = io.ReadFull(conn, payload)
	}
	return
}

func writeFrame(conn net.Conn, cmd byte, payload []byte) error {
	hdr := make([]byte, 5)
	hdr[0] = cmd
	le.PutUint32(hdr[1:], uint32(len(payload)))
	if _, err := conn.Write(hdr); err != nil {
		return err
	}
	if len(payload) > 0 {
		_, err := conn.Write(payload)
		return err
	}
	return nil
}

// ---- connection handler ---------------------------------------------------

func handleConn(conn net.Conn, cat *catalog) {
	defer conn.Close()
	addr := conn.RemoteAddr()
	log.Printf("connect: %s", addr)
	defer log.Printf("disconnect: %s", addr)

	// files snapshot for this session (updated on each LIST_FILES).
	// Ensures READ_FILE uses the same IDs that were returned by LIST_FILES.
	var sessionFiles []fileEntry

	for {
		cmd, payload, err := readFrame(conn)
		if err != nil {
			if err != io.EOF {
				log.Printf("%s read: %v", addr, err)
			}
			return
		}

		switch cmd {

		case cmdHello:
			if len(payload) < 2 {
				log.Printf("%s: HELLO payload too short", addr)
				return
			}
			clientVer := le.Uint16(payload[0:2])
			log.Printf("%s: HELLO proto_version=%d", addr, clientVer)
			resp := make([]byte, 10)
			le.PutUint16(resp[0:2], protoVersion)
			le.PutUint64(resp[2:10], cat.getDiskVersion())
			if err := writeFrame(conn, cmdHello, resp); err != nil {
				return
			}

		case cmdVersion:
			resp := make([]byte, 8)
			le.PutUint64(resp[0:8], cat.getDiskVersion())
			if err := writeFrame(conn, cmdVersion, resp); err != nil {
				return
			}

		case cmdListFiles:
			// Refresh snapshot so READ_FILE sees consistent IDs.
			sessionFiles, _ = cat.getState()
			resp := make([]byte, 2+len(sessionFiles)*23)
			le.PutUint16(resp[0:2], uint16(len(sessionFiles)))
			for i, f := range sessionFiles {
				b := 2 + i*23
				copy(resp[b:b+13], f.name[:])
				le.PutUint32(resp[b+13:b+17], f.size)
				le.PutUint32(resp[b+17:b+21], f.mtime)
				le.PutUint16(resp[b+21:b+23], f.fileID)
			}
			if err := writeFrame(conn, cmdListFiles, resp); err != nil {
				return
			}
			log.Printf("%s: LIST_FILES -> %d files", addr, len(sessionFiles))

		case cmdReadFile:
			if len(payload) < 10 {
				log.Printf("%s: READ_FILE payload too short", addr)
				return
			}
			fileID := le.Uint16(payload[0:2])
			offset := le.Uint32(payload[2:6])
			length := le.Uint32(payload[6:10])

			var entry *fileEntry
			for i := range sessionFiles {
				if sessionFiles[i].fileID == fileID {
					entry = &sessionFiles[i]
					break
				}
			}
			if entry == nil {
				log.Printf("%s: READ_FILE unknown file_id=%d", addr, fileID)
				resp := make([]byte, 4) // actual=0
				writeFrame(conn, cmdReadFile, resp)
				continue
			}

			data := make([]byte, length)
			f, err := os.Open(entry.path)
			var n int
			if err == nil {
				n, _ = f.ReadAt(data, int64(offset))
				f.Close()
			} else {
				log.Printf("%s: open %s: %v", addr, entry.path, err)
			}

			resp := make([]byte, 4+n)
			le.PutUint32(resp[0:4], uint32(n))
			copy(resp[4:], data[:n])
			if err := writeFrame(conn, cmdReadFile, resp); err != nil {
				return
			}
			log.Printf("%s: READ_FILE id=%d offset=%d len=%d -> %d bytes", addr, fileID, offset, length, n)

		default:
			log.Printf("%s: unknown cmd 0x%02x, closing", addr, cmd)
			return
		}
	}
}

// ---- directory watcher (polling, stdlib only) ----------------------------

func watchDir(dir string, cat *catalog) {
	type fileKey struct {
		name  string
		size  int64
		mtime int64
	}
	snapshot := func() map[fileKey]struct{} {
		entries, _ := os.ReadDir(dir)
		m := make(map[fileKey]struct{})
		for _, e := range entries {
			if e.IsDir() {
				continue
			}
			ext := strings.ToUpper(filepath.Ext(e.Name()))
			if !cat.allowedExt[ext] {
				continue
			}
			fi, err := e.Info()
			if err != nil {
				continue
			}
			m[fileKey{e.Name(), fi.Size(), fi.ModTime().Unix()}] = struct{}{}
		}
		return m
	}
	equal := func(a, b map[fileKey]struct{}) bool {
		if len(a) != len(b) {
			return false
		}
		for k := range a {
			if _, ok := b[k]; !ok {
				return false
			}
		}
		return true
	}

	current := snapshot()
	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		next := snapshot()
		if !equal(current, next) {
			current = next
			log.Printf("directory changed, reloading catalog")
			if err := cat.reload(); err != nil {
				log.Printf("reload error: %v", err)
			}
		}
	}
}

// ---- discovery responder ---------------------------------------------------

// runDiscoveryResponder answers UDP broadcast discovery requests from the
// embroidery stick with this backend's TCP port. The stick's IP isn't
// needed on the wire — the reply goes straight back to the sender's
// address via ReadFrom/WriteTo.
func runDiscoveryResponder(tcpPort int) {
	conn, err := net.ListenPacket("udp4", fmt.Sprintf(":%d", discoveryPort))
	if err != nil {
		log.Printf("discovery: listen failed: %v", err)
		return
	}
	defer conn.Close()
	log.Printf("discovery responder listening on :%d", discoveryPort)

	buf := make([]byte, 64)
	for {
		n, raddr, err := conn.ReadFrom(buf)
		if err != nil {
			continue
		}
		if string(buf[:n]) != discoveryMagic {
			continue
		}
		resp := make([]byte, len(discoveryMagic)+2)
		copy(resp, discoveryMagic)
		le.PutUint16(resp[len(discoveryMagic):], uint16(tcpPort))
		if _, err := conn.WriteTo(resp, raddr); err != nil {
			log.Printf("discovery: reply to %s failed: %v", raddr, err)
			continue
		}
		log.Printf("discovery: replied to %s with port %d", raddr, tcpPort)
	}
}

// ---- main -----------------------------------------------------------------

func main() {
	dir := flag.String("dir", ".", "directory with embroidery files")
	port := flag.Int("port", defaultPort, "TCP listen port")
	extConfig := flag.String("extensions", "extensions.conf", "file listing allowed file extensions (one per line, # to comment out)")
	flag.Parse()

	allowedExt, err := loadAllowedExtensions(*extConfig)
	if err != nil {
		log.Fatalf("extensions config: %v", err)
	}

	cat, err := newCatalog(*dir, allowedExt)
	if err != nil {
		log.Fatalf("catalog: %v", err)
	}

	// Watch directory for changes every 2 seconds (OS-independent, no dependencies).
	go watchDir(*dir, cat)

	// Answer backend auto-discovery broadcasts from the embroidery stick.
	go runDiscoveryResponder(*port)

	// SIGHUP triggers a catalog reload (add/remove files without restart).
	go func() {
		ch := make(chan os.Signal, 1)
		signal.Notify(ch, syscall.SIGHUP)
		for range ch {
			log.Printf("SIGHUP: reloading catalog")
			if err := cat.reload(); err != nil {
				log.Printf("reload error: %v", err)
			}
		}
	}()

	addr := fmt.Sprintf(":%d", *port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("listen %s: %v", addr, err)
	}
	log.Printf("listening on %s, serving %s", addr, *dir)

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			continue
		}
		go handleConn(conn, cat)
	}
}
