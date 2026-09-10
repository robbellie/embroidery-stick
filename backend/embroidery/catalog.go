package embroidery

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

// node mirrors proto_file_info_t (28 bytes packed on the wire) plus
// backend-local bookkeeping not sent over the wire.
type node struct {
	name     [13]byte
	size     uint32
	mtime    uint32
	id       uint16
	parentID uint16
	isDir    bool
	path     string // absolute local filesystem path (files AND directories)
}

// Catalog holds the current directory tree, flattened to mirror the wire
// format exactly (a slice of nodes with parent pointers, not a Go tree of
// pointers) so building the LIST_FILES response is a direct copy.
type Catalog struct {
	mu          sync.RWMutex
	rootDir     string
	allowedExt  map[string]bool // e.g. ".PES" -> true
	nodes       []node
	diskVersion uint64

	logf           func(format string, args ...any)
	onFileDetected func(path string, added bool)
}

// KnownExtensions lists the embroidery file formats offered as checkboxes
// by the GUI, in the order written to extensions.conf. PES is the only one
// enabled by default (see LoadAllowedExtensions).
var KnownExtensions = []string{".PES", ".DST", ".JEF", ".EXP", ".VP3", ".XXX", ".HUS"}

// LoadAllowedExtensions reads a simple newline-delimited extension list
// from path, creating it (via SaveAllowedExtensions, PES-only) if it
// doesn't exist yet. Deliberately not a TOML/YAML/JSON config —
// stdlib-only, matches the rest of this backend. Exported so the GUI can
// read the current selection before a Server (and its Catalog) exists.
func LoadAllowedExtensions(path string) (map[string]bool, error) {
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		defaultAllowed := map[string]bool{".PES": true}
		if werr := SaveAllowedExtensions(path, defaultAllowed); werr != nil {
			return nil, fmt.Errorf("could not write default %s: %w", path, werr)
		}
		return defaultAllowed, nil
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

// SaveAllowedExtensions writes path in the same self-documenting,
// hand-editable format as the auto-generated default: each of
// KnownExtensions listed bare if allowed or commented out otherwise,
// followed by any extensions in allowed that aren't in KnownExtensions
// (e.g. one a user typed into a config file by hand) so they survive
// being re-saved from the GUI's checkbox list instead of silently
// disappearing.
func SaveAllowedExtensions(path string, allowed map[string]bool) error {
	var b strings.Builder
	b.WriteString("# Embroidery Stick - allowed file types\n")
	b.WriteString("# One extension per line (leading dot optional). Lines starting with #\n")
	b.WriteString("# are ignored. Uncomment (or add) the formats your embroidery machine\n")
	b.WriteString("# supports.\n\n")

	known := make(map[string]bool, len(KnownExtensions))
	for _, ext := range KnownExtensions {
		known[ext] = true
		if allowed[ext] {
			b.WriteString(ext + "\n")
		} else {
			b.WriteString("# " + ext + "\n")
		}
	}

	var extra []string
	for ext, on := range allowed {
		if on && !known[ext] {
			extra = append(extra, ext)
		}
	}
	sort.Strings(extra)
	if len(extra) > 0 {
		b.WriteString("\n# Additional formats\n")
		for _, ext := range extra {
			b.WriteString(ext + "\n")
		}
	}

	return os.WriteFile(path, []byte(b.String()), 0o644)
}

func newCatalog(dir string, allowedExt map[string]bool, logf func(string, ...any), onFileDetected func(string, bool)) (*Catalog, error) {
	c := &Catalog{rootDir: dir, allowedExt: allowedExt, logf: logf, onFileDetected: onFileDetected}
	return c, c.reload()
}

// treeNode is the intermediate, pointer-based tree built during a walk,
// before it's sorted-and-flattened into the wire-shaped []node.
type treeNode struct {
	base, ext string // already split+uppercased; ext is "" for directories
	fi        fs.FileInfo
	path      string
	isDir     bool
	children  []*treeNode
}

// buildTree walks c.rootDir recursively (one level deep or many — no
// depth limit) and returns the root of an in-memory tree. Unreadable
// entries are skipped rather than aborting the whole walk, matching the
// original flat implementation's tolerance for a single bad entry.
// Directories are always included, even if empty or containing only
// disallowed extensions — an empty folder is still a folder.
func (c *Catalog) buildTree() (*treeNode, error) {
	allowedExt := c.getAllowedExt()
	root := &treeNode{isDir: true, path: c.rootDir}
	byPath := map[string]*treeNode{c.rootDir: root}

	err := filepath.WalkDir(c.rootDir, func(p string, d fs.DirEntry, err error) error {
		if err != nil || p == c.rootDir {
			return nil
		}
		info, ierr := d.Info()
		if ierr != nil {
			return nil
		}

		var tn *treeNode
		if d.IsDir() {
			tn = &treeNode{base: strings.ToUpper(d.Name()), fi: info, path: p, isDir: true}
		} else {
			ext := strings.ToUpper(filepath.Ext(d.Name()))
			if !allowedExt[ext] {
				return nil
			}
			tn = &treeNode{
				base: strings.ToUpper(strings.TrimSuffix(d.Name(), filepath.Ext(d.Name()))),
				ext:  strings.TrimPrefix(ext, "."),
				fi:   info,
				path: p,
			}
		}

		parent := byPath[filepath.Dir(p)]
		if parent == nil {
			return nil // shouldn't happen: WalkDir visits a directory before its contents
		}
		parent.children = append(parent.children, tn)
		if tn.isDir {
			byPath[p] = tn
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	return root, nil
}

// dirCtx disambiguates 8.3 short-name collisions within ONE directory's
// entries only — real FAT/Windows short-name uniqueness is per-directory,
// not filesystem-wide, so a global counter (the original implementation)
// would over-suffix files that happen to share a truncated name across
// different folders.
type dirCtx struct {
	seen map[string]int
}

// disambiguate8_3 truncates base to 8 chars and appends a "~N" suffix on
// collision, Windows-short-name style: the first occurrence in this
// directory keeps its plain truncated name, later collisions get a
// carved-out suffix (e.g. TAS_NATU.PES, TAS_NA~1.PES, TAS_NA~2.PES, ...).
func disambiguate8_3(dctx *dirCtx, base, ext string) [13]byte {
	if len(base) > 8 {
		base = base[:8]
	}
	key := base + "." + ext
	dctx.seen[key]++
	name := base
	if n := dctx.seen[key]; n > 1 {
		suffix := fmt.Sprintf("~%d", n-1)
		cut := len(name) - len(suffix)
		if cut < 0 {
			cut = 0
		}
		name = name[:cut] + suffix
	}
	fname := name
	if ext != "" {
		fname = name + "." + ext
	}
	var out [13]byte
	copy(out[:], fname)
	return out
}

// flattenTree sorts each directory's direct children alphabetically among
// themselves (never a single global sort across the whole tree, which
// would scramble the parent-before-child ordering the wire format
// requires) and flattens depth-first, assigning sequential ids as it
// goes. Since a directory's own id is assigned before recursing into its
// children, every child's parentID is already known when it's emitted —
// satisfying the wire's "parent before child" contract with a stronger
// guarantee than required (full pre-order, not just immediate-parent-first).
//
// With zero subdirectories this produces byte-identical ordering/ids to
// the original flat implementation's single global sort.
func flattenTree(root *treeNode) []node {
	var out []node
	var walk func(tn *treeNode, parentID uint16)
	walk = func(tn *treeNode, parentID uint16) {
		sort.Slice(tn.children, func(i, j int) bool {
			a, b := tn.children[i], tn.children[j]
			return strings.ToLower(a.base+"."+a.ext) < strings.ToLower(b.base+"."+b.ext)
		})

		dctx := &dirCtx{seen: map[string]int{}}
		for _, child := range tn.children {
			n := node{
				parentID: parentID,
				isDir:    child.isDir,
				path:     child.path,
				name:     disambiguate8_3(dctx, child.base, child.ext),
				mtime:    uint32(child.fi.ModTime().Unix()),
			}
			if !child.isDir {
				n.size = uint32(child.fi.Size())
			}
			n.id = uint16(len(out))
			out = append(out, n)

			if child.isDir {
				walk(child, n.id)
			}
		}
	}
	walk(root, RootParentID)
	return out
}

// reload re-walks the directory tree and updates nodes + diskVersion.
func (c *Catalog) reload() error {
	root, err := c.buildTree()
	if err != nil {
		return fmt.Errorf("walk %s: %w", c.rootDir, err)
	}
	nodes := flattenTree(root)

	c.mu.Lock()
	c.nodes = nodes
	c.diskVersion = uint64(time.Now().UnixNano())
	c.mu.Unlock()

	c.logf("catalog reloaded: %d entries, diskVersion=%d", len(nodes), c.diskVersion)
	return nil
}

func (c *Catalog) getState() (nodes []node, diskVersion uint64) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	out := make([]node, len(c.nodes))
	copy(out, c.nodes)
	return out, c.diskVersion
}

func (c *Catalog) getDiskVersion() uint64 {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.diskVersion
}

func (c *Catalog) getAllowedExt() map[string]bool {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.allowedExt
}

// SetAllowedExtensions replaces the set of served file extensions and
// re-scans the directory immediately. Unlike the served folder or port,
// this is safe to change on a running server — it doesn't touch any
// socket, just which files the existing catalog considers visible.
func (c *Catalog) SetAllowedExtensions(allowed map[string]bool) error {
	c.mu.Lock()
	c.allowedExt = allowed
	c.mu.Unlock()
	return c.reload()
}
