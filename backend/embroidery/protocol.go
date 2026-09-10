// Package embroidery implements the embroidery-stick backend: a directory
// catalog (with subdirectory support), the binary TCP protocol the
// firmware speaks, and UDP auto-discovery — as an importable library so
// both a headless CLI and a GUI can embed the same server.
package embroidery

import (
	"encoding/binary"
	"io"
	"net"
)

/*
 * Protocol (little-endian, all frames):
 *   [cmd:1][payload_len:4][payload:N]
 *
 * CMD_HELLO      (0x01) req: [proto_version:2]
 *                       res: [proto_version:2][disk_version:8]
 * CMD_VERSION    (0x02) req: no payload
 *                       res: [disk_version:8]
 * CMD_LIST_FILES (0x03) req: no payload
 *                       res: [count:2][{name[13], size:4, mtime:4, id:2,
 *                             parent_id:2, is_dir:1, reserved[2]} × count]
 *                       Producer ordering contract: every directory node
 *                       must appear before any child that references it
 *                       via parent_id.
 * CMD_READ_FILE  (0x04) req: [id:2][offset:4][length:4] — id must be a
 *                       file, never a directory (see RootParentID doc).
 *                       res: [actual:4][data × actual]
 *
 * Matches main/embroidery_protocol.h — keep both in sync by hand (no
 * codegen; this backend is deliberately stdlib-only).
 */

const (
	ProtoVersion = uint16(2) // v2: node record widened for directory-tree support
	DefaultPort  = 7892

	// RootParentID marks a node living directly in the served root
	// directory, matching EMBROIDERY_ROOT_PARENT_ID in the C header.
	RootParentID = uint16(0xFFFF)

	cmdHello     = byte(0x01)
	cmdVersion   = byte(0x02)
	cmdListFiles = byte(0x03)
	cmdReadFile  = byte(0x04)

	// nodeWireSize is proto_file_info_t's exact packed size (13+4+4+2+2+1+2).
	nodeWireSize = 28

	// Backend auto-discovery: one-shot UDP broadcast/reply, separate from
	// the TCP control connection above. These must match the firmware's
	// EMBROIDERY_DISCOVERY_MAGIC/EMBROIDERY_DISCOVERY_DEFAULT_PORT in
	// embroidery_protocol.h — duplicated as matching literals since Go
	// can't include that C header (same pattern as DefaultPort above).
	discoveryMagic = "EMBROIDERY_DISCOVER_V1"
	discoveryPort  = 7891
)

var le = binary.LittleEndian

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
