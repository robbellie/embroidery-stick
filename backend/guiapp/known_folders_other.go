//go:build !windows

package guiapp

// knownFolderPath is Windows-only — Known Folder Move / OneDrive folder
// redirection is a Windows-specific concept, so there's nothing to
// resolve elsewhere. Always reports not found, so callers fall back to
// Fyne's normal cross-platform folder dialog behavior.
func knownFolderPath(knownFolder) (path string, ok bool) {
	return "", false
}
