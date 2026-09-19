//go:build windows

package guiapp

import "golang.org/x/sys/windows"

// knownFolderPath resolves kf via SHGetKnownFolderPath, which honors
// Known Folder Move — unlike a hard-coded %USERPROFILE%\Desktop guess,
// this returns the actual OneDrive-redirected location when that's in
// effect. ok is false if the lookup fails (shouldn't normally happen for
// these two well-known folders, but never worth a hard failure just to
// seed a dialog's starting location).
func knownFolderPath(kf knownFolder) (path string, ok bool) {
	var id *windows.KNOWNFOLDERID
	switch kf {
	case knownFolderDesktop:
		id = windows.FOLDERID_Desktop
	case knownFolderDocuments:
		id = windows.FOLDERID_Documents
	default:
		return "", false
	}
	p, err := windows.KnownFolderPath(id, windows.KF_FLAG_DEFAULT)
	if err != nil {
		return "", false
	}
	return p, true
}
