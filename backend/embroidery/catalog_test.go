package embroidery

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func mustWriteFile(t *testing.T, path string, size int) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, make([]byte, size), 0o644); err != nil {
		t.Fatal(err)
	}
}

func mustMkdir(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(path, 0o755); err != nil {
		t.Fatal(err)
	}
}

func nodesByPath(nodes []node) map[string]node {
	m := make(map[string]node, len(nodes))
	for _, n := range nodes {
		m[n.path] = n
	}
	return m
}

func nameStr(n node) string {
	return strings.TrimRight(string(n.name[:]), "\x00")
}

func buildCatalog(t *testing.T, dir string) []node {
	t.Helper()
	c := &Catalog{
		rootDir:    dir,
		allowedExt: map[string]bool{".PES": true},
		logf:       func(string, ...any) {},
	}
	if err := c.reload(); err != nil {
		t.Fatalf("reload: %v", err)
	}
	nodes, _ := c.getState()
	return nodes
}

// verifyParentBeforeChild checks the wire ordering contract: no node may
// reference a parent_id that hasn't already appeared earlier in the slice.
func verifyParentBeforeChild(t *testing.T, nodes []node) {
	t.Helper()
	seen := map[uint16]bool{}
	for i, n := range nodes {
		if n.parentID != RootParentID && !seen[n.parentID] {
			t.Fatalf("node %d (%q) references parent_id %d before it was emitted", i, nameStr(n), n.parentID)
		}
		seen[n.id] = true
	}
}

func TestFlatDirectory(t *testing.T) {
	dir := t.TempDir()
	mustWriteFile(t, filepath.Join(dir, "rose.pes"), 100)
	mustWriteFile(t, filepath.Join(dir, "logo.pes"), 200)

	nodes := buildCatalog(t, dir)
	if len(nodes) != 2 {
		t.Fatalf("want 2 nodes, got %d", len(nodes))
	}
	for _, n := range nodes {
		if n.isDir {
			t.Fatalf("unexpected directory node: %+v", n)
		}
		if n.parentID != RootParentID {
			t.Fatalf("flat file should have RootParentID, got %d", n.parentID)
		}
	}
	verifyParentBeforeChild(t, nodes)
}

func TestOneLevelNesting(t *testing.T) {
	dir := t.TempDir()
	mustWriteFile(t, filepath.Join(dir, "top.pes"), 10)
	mustWriteFile(t, filepath.Join(dir, "Patterns", "rose.pes"), 20)

	nodes := buildCatalog(t, dir)
	byPath := nodesByPath(nodes)

	dirNode, ok := byPath[filepath.Join(dir, "Patterns")]
	if !ok || !dirNode.isDir {
		t.Fatalf("expected a directory node for Patterns, got %+v", dirNode)
	}
	if dirNode.parentID != RootParentID {
		t.Fatalf("Patterns should be a root child, got parent_id=%d", dirNode.parentID)
	}

	fileNode, ok := byPath[filepath.Join(dir, "Patterns", "rose.pes")]
	if !ok || fileNode.isDir {
		t.Fatalf("expected a file node for rose.pes, got %+v", fileNode)
	}
	if fileNode.parentID != dirNode.id {
		t.Fatalf("rose.pes parent_id=%d, want %d (Patterns' id)", fileNode.parentID, dirNode.id)
	}
	verifyParentBeforeChild(t, nodes)
}

func TestEmptySubdirectory(t *testing.T) {
	dir := t.TempDir()
	mustMkdir(t, filepath.Join(dir, "Empty"))

	nodes := buildCatalog(t, dir)
	if len(nodes) != 1 {
		t.Fatalf("want 1 node (the empty dir itself), got %d", len(nodes))
	}
	if !nodes[0].isDir {
		t.Fatalf("expected a directory node, got %+v", nodes[0])
	}
}

func TestDisambiguationIsPerDirectoryNotGlobal(t *testing.T) {
	dir := t.TempDir()
	// Two files that both truncate to the same 8.3 base, but in DIFFERENT
	// directories — must NOT collide with each other.
	mustWriteFile(t, filepath.Join(dir, "FolderA", "verylongname1.pes"), 10)
	mustWriteFile(t, filepath.Join(dir, "FolderB", "verylongname2.pes"), 10)
	// Two files in the SAME directory that DO collide — must disambiguate.
	mustWriteFile(t, filepath.Join(dir, "FolderA", "verylongname3.pes"), 10)

	nodes := buildCatalog(t, dir)
	byPath := nodesByPath(nodes)

	a := nameStr(byPath[filepath.Join(dir, "FolderA", "verylongname1.pes")])
	b := nameStr(byPath[filepath.Join(dir, "FolderB", "verylongname2.pes")])
	if a != b {
		t.Errorf("files in different directories should truncate identically when nothing in their own directory collides: got %q and %q", a, b)
	}
	if a != "VERYLONG.PES" {
		t.Errorf("expected plain truncated name VERYLONG.PES for the first file in its directory, got %q", a)
	}

	c := nameStr(byPath[filepath.Join(dir, "FolderA", "verylongname3.pes")])
	if c == a {
		t.Errorf("second colliding file in the SAME directory must be disambiguated, both got %q", c)
	}
	if !strings.Contains(c, "~") {
		t.Errorf("expected a ~N disambiguation suffix, got %q", c)
	}
}

func TestDeeplyNestedPath(t *testing.T) {
	dir := t.TempDir()
	deep := filepath.Join(dir, "A", "B", "C", "D")
	mustWriteFile(t, filepath.Join(deep, "pattern.pes"), 5)

	nodes := buildCatalog(t, dir)
	verifyParentBeforeChild(t, nodes)

	byPath := nodesByPath(nodes)
	fileNode, ok := byPath[filepath.Join(deep, "pattern.pes")]
	if !ok {
		t.Fatalf("deeply nested file not found in catalog")
	}

	// Walk parent_id chain back to root, confirming it terminates and
	// matches the actual directory nesting depth (A -> B -> C -> D -> file).
	depth := 0
	cur := fileNode
	idToNode := map[uint16]node{}
	for _, n := range nodes {
		idToNode[n.id] = n
	}
	for cur.parentID != RootParentID {
		parent, ok := idToNode[cur.parentID]
		if !ok {
			t.Fatalf("dangling parent_id %d", cur.parentID)
		}
		cur = parent
		depth++
		if depth > 10 {
			t.Fatalf("parent_id chain didn't terminate — possible cycle")
		}
	}
	if depth != 4 {
		t.Fatalf("expected nesting depth 4 (A/B/C/D), got %d", depth)
	}
}

func TestDirectoryMtimeSet(t *testing.T) {
	dir := t.TempDir()
	mustMkdir(t, filepath.Join(dir, "Sub"))

	nodes := buildCatalog(t, dir)
	if nodes[0].mtime == 0 {
		t.Errorf("directory node should have a non-zero mtime")
	}
	if nodes[0].size != 0 {
		t.Errorf("directory node should always report size 0, got %d", nodes[0].size)
	}
}
