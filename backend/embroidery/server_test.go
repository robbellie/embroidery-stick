package embroidery

import (
	"net"
	"path/filepath"
	"strings"
	"testing"
)

// Regression test for a real report: an embroidery machine reads a file
// twice (a quick pass for a listing/thumbnail, then a full pass for real
// use). A naive running sum of bytes read double-counts the overlap,
// which can cross the file's size early, log "transfer complete" mid-way
// through the real read, and then never see the real read's tail end
// finish — a file that visibly keeps being read but never completes.
func TestHandleConnDedupesOverlappingReads(t *testing.T) {
	dir := t.TempDir()
	const fileSize = 20000 // several 8KB-ish chunks, not a round multiple
	mustWriteFile(t, filepath.Join(dir, "TEST.PES"), fileSize)

	cat, err := newCatalog(dir, map[string]bool{".PES": true}, func(string, ...any) {}, nil)
	if err != nil {
		t.Fatalf("newCatalog: %v", err)
	}

	var logs []string
	s := &Server{
		cfg: Config{Dir: dir, OnLog: func(msg string) { logs = append(logs, msg) }},
		cat: cat,
	}

	clientConn, serverConn := net.Pipe()
	done := make(chan struct{})
	go func() {
		s.handleConn(serverConn)
		close(done)
	}()

	send := func(cmd byte, payload []byte) {
		t.Helper()
		if err := writeFrame(clientConn, cmd, payload); err != nil {
			t.Fatalf("writeFrame: %v", err)
		}
	}
	recv := func() (byte, []byte) {
		t.Helper()
		cmd, payload, err := readFrame(clientConn)
		if err != nil {
			t.Fatalf("readFrame: %v", err)
		}
		return cmd, payload
	}

	send(cmdHello, func() []byte {
		b := make([]byte, 2)
		le.PutUint16(b, ProtoVersion)
		return b
	}())
	recv() // HELLO response

	send(cmdListFiles, nil)
	_, payload := recv()
	count := le.Uint16(payload[0:2])
	if count != 1 {
		t.Fatalf("expected 1 file in catalog, got %d", count)
	}
	fileID := le.Uint16(payload[2+21 : 2+23]) // node record: id at offset 21

	readFile := func(offset, length uint32) uint32 {
		t.Helper()
		req := make([]byte, 10)
		le.PutUint16(req[0:2], fileID)
		le.PutUint32(req[2:6], offset)
		le.PutUint32(req[6:10], length)
		send(cmdReadFile, req)
		_, resp := recv()
		return le.Uint32(resp[0:4])
	}

	// Pass 1: thumbnail-style partial read — just the first chunk.
	if n := readFile(0, 8192); n != 8192 {
		t.Fatalf("pass 1 read: got %d bytes, want 8192", n)
	}

	// Pass 2: full sequential read, starting from offset 0 again — this is
	// exactly the overlap that must not be double-counted.
	var offset uint32
	for offset < fileSize {
		n := readFile(offset, 8192)
		if n == 0 {
			t.Fatalf("pass 2 read at offset %d returned 0 bytes before reaching fileSize", offset)
		}
		offset += n
	}

	clientConn.Close()
	<-done

	var completions []string
	for _, l := range logs {
		if strings.Contains(l, "transfer complete") {
			completions = append(completions, l)
		}
	}
	if len(completions) != 1 {
		t.Fatalf("expected exactly 1 \"transfer complete\" log line, got %d: %v", len(completions), completions)
	}
	want := "20000 bytes"
	if !strings.Contains(completions[0], want) {
		t.Fatalf("transfer complete line %q does not report %s (dedup likely wrong)", completions[0], want)
	}
}
