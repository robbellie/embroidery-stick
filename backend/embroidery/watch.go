package embroidery

import (
	"context"
	"io/fs"
	"path/filepath"
	"strings"
	"time"
)

// fileKey identifies a filesystem entry for change detection. relPath
// (not just name) is required once subdirectories exist — two same-named
// files in different folders would otherwise collide in the diff.
type fileKey struct {
	relPath string
	size    int64
	mtime   int64
}

// snapshotTree walks dir recursively (directories included in the result
// set too, with size 0, so an added/removed empty subfolder is detected
// even before any file lands in it) and returns a set of fileKeys.
func snapshotTree(dir string, allowedExt map[string]bool) map[fileKey]struct{} {
	m := make(map[fileKey]struct{})
	_ = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil || p == dir {
			return nil
		}
		info, ierr := d.Info()
		if ierr != nil {
			return nil
		}
		rel, rerr := filepath.Rel(dir, p)
		if rerr != nil {
			return nil
		}
		if d.IsDir() {
			m[fileKey{relPath: rel, size: 0, mtime: info.ModTime().Unix()}] = struct{}{}
			return nil
		}
		ext := strings.ToUpper(filepath.Ext(d.Name()))
		if !allowedExt[ext] {
			return nil
		}
		m[fileKey{relPath: rel, size: info.Size(), mtime: info.ModTime().Unix()}] = struct{}{}
		return nil
	})
	return m
}

func snapshotsEqual(a, b map[fileKey]struct{}) bool {
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

// watchDir polls the served directory every 2 seconds (OS-independent,
// stdlib only — deliberately not fsnotify/inotify) and reloads the
// catalog on any change, firing onFileDetected for each individual
// added/removed path so a GUI can show live "new file" notifications
// rather than just "something changed."
func (s *Server) watchDir(ctx context.Context) {
	current := snapshotTree(s.cfg.Dir, s.cat.getAllowedExt())
	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			next := snapshotTree(s.cfg.Dir, s.cat.getAllowedExt())
			if snapshotsEqual(current, next) {
				continue
			}
			if s.cfg.OnFileDetected != nil {
				for k := range next {
					if _, ok := current[k]; !ok {
						s.cfg.OnFileDetected(k.relPath, true)
					}
				}
				for k := range current {
					if _, ok := next[k]; !ok {
						s.cfg.OnFileDetected(k.relPath, false)
					}
				}
			}
			current = next
			s.logf("directory changed, reloading catalog")
			if err := s.cat.reload(); err != nil {
				s.logf("reload error: %v", err)
			}
		}
	}
}
