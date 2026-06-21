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
	files       []fileEntry
	diskVersion uint64
}

func newCatalog(dir string) (*catalog, error) {
	c := &catalog{dir: dir}
	return c, c.reload()
}

// reload reads the directory and updates files + diskVersion if anything changed.
func (c *catalog) reload() error {
	entries, err := os.ReadDir(c.dir)
	if err != nil {
		return fmt.Errorf("readdir %s: %w", c.dir, err)
	}

	var files []fileEntry
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		ext := strings.ToUpper(filepath.Ext(e.Name()))
		if ext != ".PES" && ext != ".DST" && ext != ".JEF" {
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
		fname := base + "." + strings.TrimPrefix(ext, ".")

		var nameArr [13]byte
		copy(nameArr[:], fname)

		files = append(files, fileEntry{
			name:  nameArr,
			size:  uint32(fi.Size()),
			mtime: uint32(fi.ModTime().Unix()),
			path:  filepath.Join(c.dir, e.Name()),
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
			if ext != ".PES" && ext != ".DST" && ext != ".JEF" {
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

// ---- main -----------------------------------------------------------------

func main() {
	dir := flag.String("dir", ".", "directory with .PES/.DST/.JEF files")
	port := flag.Int("port", defaultPort, "TCP listen port")
	flag.Parse()

	cat, err := newCatalog(*dir)
	if err != nil {
		log.Fatalf("catalog: %v", err)
	}

	// Watch directory for changes every 2 seconds (OS-independent, no dependencies).
	go watchDir(*dir, cat)

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
