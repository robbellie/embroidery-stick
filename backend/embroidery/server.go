package embroidery

import (
	"context"
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

// Config configures a Server. Dir, Port, and ExtConfigPath are read once
// at New() and fixed for the server's lifetime — changing them requires a
// Stop() + New() + Start() cycle (matches the CLI's own one-shot-flags
// model; no hot config swap in v1).
type Config struct {
	Dir           string // root directory to serve
	Port          int    // TCP listen port; 0 or unset uses DefaultPort
	ExtConfigPath string // path to the allowed-extensions file; "" uses "extensions.conf"

	// Status hooks — all optional (nil-checked before use), invoked from
	// arbitrary goroutines (a per-connection goroutine for OnFileRead, the
	// watcher goroutine for OnFileDetected/reload-triggered OnLog calls),
	// so callback bodies must be safe for concurrent invocation and must
	// not block for long (they run inline on the goroutine that detected
	// the event). A GUI wiring these up must marshal onto its own UI
	// thread itself (e.g. Fyne's fyne.Do) — this package has no UI
	// framework opinion.
	OnFileRead     func(path string, offset, length uint32)
	OnFileDetected func(path string, added bool) // added=false => removed
	OnLog          func(msg string)              // catch-all; nil => real log.Printf (CLI default)
}

// Server is a running (or not-yet-started) embroidery backend: the TCP
// control connection, UDP auto-discovery responder, and the directory
// watcher, all under one Start()/Stop() lifecycle.
type Server struct {
	cfg Config
	cat *Catalog

	mu      sync.Mutex
	started bool
	ln      net.Listener
	udp     net.PacketConn
	cancel  context.CancelFunc
	wg      sync.WaitGroup

	// connMu/conns track every currently-accepted connection so Stop() can
	// close them explicitly. Without this, closing just the listener
	// leaves already-accepted connections (and their handleConn goroutine)
	// running indefinitely — the stick's persistent connection would keep
	// happily answering CMD_VERSION/CMD_LIST_FILES forever, so pressing
	// Stop in the GUI would never actually look offline to a client.
	connMu sync.Mutex
	conns  map[net.Conn]struct{}
}

func (s *Server) logf(format string, args ...any) {
	if s.cfg.OnLog != nil {
		s.cfg.OnLog(fmt.Sprintf(format, args...))
		return
	}
	log.Printf(format, args...)
}

// Logs exactly which extensions.conf the CLI/GUI resolved (as an absolute
// path — ExtConfigPath is normally relative, so the CLI and GUI can
// silently end up reading two different files if launched from different
// working directories) and what's actually enabled in it. Added after a
// real report of the GUI silently serving 0 files from a folder the CLI
// served fine from the same machine, with no visible reason why.
func (s *Server) logExtConfig(allowedExt map[string]bool) {
	abs, err := filepath.Abs(s.cfg.ExtConfigPath)
	if err != nil {
		abs = s.cfg.ExtConfigPath
	}
	var enabled []string
	for ext, on := range allowedExt {
		if on {
			enabled = append(enabled, ext)
		}
	}
	sort.Strings(enabled)
	s.logf("extensions config: %s (enabled: %s)", abs, strings.Join(enabled, ", "))
}

// New validates cfg, loads the extensions file, and builds the initial
// catalog. It does not bind any network resources yet — call Start() for
// that. Replaces the previous log.Fatalf-on-error CLI behavior with
// returned errors, so a GUI can show them instead of the process dying.
func New(cfg Config) (*Server, error) {
	if cfg.Port == 0 {
		cfg.Port = DefaultPort
	}
	if cfg.ExtConfigPath == "" {
		cfg.ExtConfigPath = "extensions.conf"
	}
	if cfg.Dir == "" {
		cfg.Dir = "."
	}

	s := &Server{cfg: cfg}

	allowedExt, err := LoadAllowedExtensions(cfg.ExtConfigPath)
	if err != nil {
		return nil, fmt.Errorf("extensions config: %w", err)
	}
	s.logExtConfig(allowedExt)

	cat, err := newCatalog(cfg.Dir, allowedExt, s.logf, cfg.OnFileDetected)
	if err != nil {
		return nil, fmt.Errorf("catalog: %w", err)
	}
	s.cat = cat
	return s, nil
}

// Start binds the TCP and UDP listeners and launches the accept loop,
// discovery responder, and directory watcher in the background. Returns
// once both listeners are bound; the accept loop itself continues
// asynchronously.
func (s *Server) Start() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.started {
		return fmt.Errorf("already started")
	}

	addr := fmt.Sprintf(":%d", s.cfg.Port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("listen %s: %w", addr, err)
	}

	udp, err := net.ListenPacket("udp4", fmt.Sprintf(":%d", discoveryPort))
	if err != nil {
		ln.Close()
		return fmt.Errorf("discovery listen: %w", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	s.ln = ln
	s.udp = udp
	s.cancel = cancel
	s.started = true
	s.conns = make(map[net.Conn]struct{})

	s.wg.Add(3)
	go func() { defer s.wg.Done(); s.acceptLoop(ctx) }()
	go func() { defer s.wg.Done(); s.discoveryLoop(ctx) }()
	go func() { defer s.wg.Done(); s.watchDir(ctx) }()

	s.logf("listening on %s, serving %s", ln.Addr(), s.cfg.Dir)
	return nil
}

// Stop cancels all background work and blocks until every goroutine has
// exited and both sockets are released — so a caller can immediately
// Start() again (e.g. with a different folder) without an "address
// already in use" race. Idempotent: calling Stop on an already-stopped
// (or never-started) Server is a no-op.
func (s *Server) Stop() error {
	s.mu.Lock()
	if !s.started {
		s.mu.Unlock()
		return nil
	}
	s.started = false
	cancel := s.cancel
	ln := s.ln
	udp := s.udp
	s.mu.Unlock()

	cancel()
	ln.Close()
	udp.Close()

	// Close every already-accepted connection too — otherwise a client
	// with a persistent connection (like the firmware) never sees Stop()
	// happen at all: its socket stays open and handleConn keeps answering
	// requests on it forever, closing only the listener for *new*
	// connections.
	s.connMu.Lock()
	for c := range s.conns {
		c.Close()
	}
	s.connMu.Unlock()

	s.wg.Wait()
	return nil
}

// Reload forces an immediate directory re-scan, independent of the
// watcher's own polling interval. Replaces the CLI-only SIGHUP handler
// for embedded (GUI) use, where there's no terminal to send a signal to.
func (s *Server) Reload() error {
	return s.cat.reload()
}

// SetAllowedExtensions updates which file extensions are served and
// re-scans the directory immediately — safe to call whether or not
// Start() has been called, and doesn't require restarting the server.
func (s *Server) SetAllowedExtensions(allowed map[string]bool) error {
	return s.cat.SetAllowedExtensions(allowed)
}

func (s *Server) DiskVersion() uint64 {
	return s.cat.getDiskVersion()
}

// Addr returns the TCP listen address, or nil if not started.
func (s *Server) Addr() net.Addr {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.ln == nil {
		return nil
	}
	return s.ln.Addr()
}

func (s *Server) acceptLoop(ctx context.Context) {
	for {
		conn, err := s.ln.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return // Stop() closed the listener; expected, not an error
			}
			s.logf("accept: %v", err)
			continue
		}
		s.connMu.Lock()
		s.conns[conn] = struct{}{}
		s.connMu.Unlock()
		go s.handleConn(conn)
	}
}

func (s *Server) discoveryLoop(ctx context.Context) {
	buf := make([]byte, 64)
	for {
		n, raddr, err := s.udp.ReadFrom(buf)
		if err != nil {
			if ctx.Err() != nil {
				return // Stop() closed the socket; expected, not an error
			}
			continue
		}
		if string(buf[:n]) != discoveryMagic {
			continue
		}
		resp := make([]byte, len(discoveryMagic)+2)
		copy(resp, discoveryMagic)
		le.PutUint16(resp[len(discoveryMagic):], uint16(s.cfg.Port))
		if _, err := s.udp.WriteTo(resp, raddr); err != nil {
			s.logf("discovery: reply to %s failed: %v", raddr, err)
			continue
		}
		s.logf("discovery: replied to %s with port %d", raddr, s.cfg.Port)
	}
}

func (s *Server) handleConn(conn net.Conn) {
	defer conn.Close()
	defer func() {
		s.connMu.Lock()
		delete(s.conns, conn)
		s.connMu.Unlock()
	}()
	addr := conn.RemoteAddr()
	s.logf("connect: %s", addr)
	defer s.logf("disconnect: %s", addr)

	// Session snapshot, refreshed on each LIST_FILES — ensures READ_FILE
	// uses the same ids that were returned by the most recent LIST_FILES
	// on this connection (ids are not stable across catalog reloads).
	var sessionNodes []node

	// Per-file transfer timing, for measuring the real-world impact of
	// firmware/network changes (WiFi power-save, TCP_NODELAY, chunk size,
	// ...) — logged once a file's bytes are fully accounted for, so an A/B
	// comparison is just: reflash, transfer the same file, compare the
	// logged KB/s. transferSeenOffsets dedupes by request offset before
	// adding to transferBytes: embroidery machines commonly read a file
	// twice (once for a quick thumbnail/listing pass, once for real use),
	// and a plain running sum double-counts that overlap — which could
	// cross entry.size early, report "complete" mid-transfer, delete the
	// tracking state, and then never see the real transfer's tail end
	// finish (a file that never gets logged as complete despite the
	// machine visibly still reading it).
	transferStart := map[uint16]time.Time{}
	transferBytes := map[uint16]uint32{}
	transferSeenOffsets := map[uint16]map[uint32]bool{}

	// Diagnoses whether a slow multi-file transfer is us being slow to
	// respond, or the client (embroidery machine or OS) simply not asking
	// for the next chunk yet — nothing in this server throttles or delays a
	// response once a request arrives, so any real gap can only mean the
	// client hadn't sent the next request. lastFrameAt is set as soon as a
	// frame is read, before any processing, so the measured gap is purely
	// "time with nothing incoming," not our own handling time.
	const readFileIdleLogThreshold = 50 * time.Millisecond
	lastFrameAt := time.Now()

	// Aggregates a whole "browsing burst" — e.g. the embroidery machine (or
	// a host OS) reading through many files back-to-back while generating
	// thumbnails for a folder — into one summary line, since the per-file
	// lines above only ever show one file at a time. The protocol has no
	// explicit "I'm done browsing" signal, so a burst is heuristically
	// considered over once the connection goes quiet for batchIdleThreshold
	// — long enough not to be mistaken for a between-file pause, short
	// enough to close out promptly once browsing has actually stopped.
	const batchIdleThreshold = 3 * time.Second
	var batchStart time.Time
	batchBytes := uint32(0)
	batchFiles := map[uint16]bool{}

	logBatch := func() {
		if batchStart.IsZero() || len(batchFiles) == 0 {
			return
		}
		elapsed := time.Since(batchStart)
		kbps := float64(batchBytes) / 1024 / elapsed.Seconds()
		s.logf("%s: batch complete: %d files, %d bytes in %v, %.1f KB/s average",
			addr, len(batchFiles), batchBytes, elapsed.Round(time.Millisecond), kbps)
		batchStart = time.Time{}
		batchBytes = 0
		batchFiles = map[uint16]bool{}
	}
	defer logBatch() // flush a still-open batch if the connection just closes

	for {
		cmd, payload, err := readFrame(conn)
		if err != nil {
			return
		}
		gapSinceLastFrame := time.Since(lastFrameAt)
		lastFrameAt = time.Now()

		switch cmd {
		case cmdHello:
			if len(payload) < 2 {
				s.logf("%s: HELLO payload too short", addr)
				return
			}
			clientVer := le.Uint16(payload[0:2])
			s.logf("%s: HELLO proto_version=%d", addr, clientVer)
			resp := make([]byte, 10)
			le.PutUint16(resp[0:2], ProtoVersion)
			le.PutUint64(resp[2:10], s.cat.getDiskVersion())
			if err := writeFrame(conn, cmdHello, resp); err != nil {
				return
			}

		case cmdVersion:
			resp := make([]byte, 8)
			le.PutUint64(resp[0:8], s.cat.getDiskVersion())
			if err := writeFrame(conn, cmdVersion, resp); err != nil {
				return
			}

		case cmdListFiles:
			sessionNodes, _ = s.cat.getState()
			resp := make([]byte, 2+len(sessionNodes)*nodeWireSize)
			le.PutUint16(resp[0:2], uint16(len(sessionNodes)))
			for i, n := range sessionNodes {
				b := 2 + i*nodeWireSize
				copy(resp[b:b+13], n.name[:])
				le.PutUint32(resp[b+13:b+17], n.size)
				le.PutUint32(resp[b+17:b+21], n.mtime)
				le.PutUint16(resp[b+21:b+23], n.id)
				le.PutUint16(resp[b+23:b+25], n.parentID)
				if n.isDir {
					resp[b+25] = 1
				}
				// resp[b+26:b+28] reserved, left zero
			}
			if err := writeFrame(conn, cmdListFiles, resp); err != nil {
				return
			}
			s.logf("%s: LIST_FILES -> %d entries", addr, len(sessionNodes))

		case cmdReadFile:
			if gapSinceLastFrame > readFileIdleLogThreshold {
				s.logf("%s: idle %v before this request (client hadn't asked yet)",
					addr, gapSinceLastFrame.Round(time.Millisecond))
			}
			if len(payload) < 10 {
				s.logf("%s: READ_FILE payload too short", addr)
				return
			}
			id := le.Uint16(payload[0:2])
			offset := le.Uint32(payload[2:6])
			length := le.Uint32(payload[6:10])

			var entry *node
			for i := range sessionNodes {
				if sessionNodes[i].id == id {
					entry = &sessionNodes[i]
					break
				}
			}
			if entry == nil || entry.isDir {
				if entry != nil && entry.isDir {
					s.logf("%s: READ_FILE requested on directory id=%d", addr, id)
				} else {
					s.logf("%s: READ_FILE unknown id=%d", addr, id)
				}
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
				s.logf("%s: open %s: %v", addr, entry.path, err)
			}
			if s.cfg.OnFileRead != nil {
				s.cfg.OnFileRead(splitRelDir(s.cfg.Dir, entry.path), offset, uint32(n))
			}

			resp := make([]byte, 4+n)
			le.PutUint32(resp[0:4], uint32(n))
			copy(resp[4:], data[:n])
			if err := writeFrame(conn, cmdReadFile, resp); err != nil {
				return
			}
			s.logf("%s: READ_FILE id=%d offset=%d len=%d -> %d bytes", addr, id, offset, length, n)

			if n > 0 {
				if !batchStart.IsZero() && gapSinceLastFrame > batchIdleThreshold {
					logBatch() // this request starts a new burst — close out the previous one first
				}
				if batchStart.IsZero() {
					batchStart = time.Now()
				}
				batchBytes += uint32(n)
				batchFiles[id] = true

				if _, started := transferStart[id]; !started {
					transferStart[id] = time.Now()
					transferSeenOffsets[id] = map[uint32]bool{}
				}
				if !transferSeenOffsets[id][offset] {
					transferSeenOffsets[id][offset] = true
					transferBytes[id] += uint32(n)
				}
				if transferBytes[id] >= entry.size {
					elapsed := time.Since(transferStart[id])
					kbps := float64(transferBytes[id]) / 1024 / elapsed.Seconds()
					s.logf("%s: transfer complete: %s (%d bytes in %v, %.1f KB/s)",
						addr, splitRelDir(s.cfg.Dir, entry.path), transferBytes[id], elapsed.Round(time.Millisecond), kbps)
					delete(transferStart, id)
					delete(transferBytes, id)
					delete(transferSeenOffsets, id)
				}
			}

		default:
			s.logf("%s: unknown cmd 0x%02x, closing", addr, cmd)
			return
		}
	}
}

// splitRelDir is a small helper for display purposes (e.g. a GUI showing
// "Patterns/Rose.pes" rather than an absolute path) — not required by the
// wire protocol, which only ever deals in 8.3 short names + parent ids.
func splitRelDir(root, path string) string {
	rel := strings.TrimPrefix(path, root)
	return strings.TrimPrefix(rel, string(os.PathSeparator))
}
