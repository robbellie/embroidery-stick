package guiapp

// knownFolder identifies a Windows "known folder" whose real filesystem
// path can differ from a naive %USERPROFILE%\<name> guess once Known
// Folder Move (e.g. OneDrive's "Back up your folders" feature) redirects
// it elsewhere — e.g. Desktop becoming
// C:\Users\<u>\OneDrive\Desktop or C:\Users\<u>\OneDrive - <Company>\Desktop.
//
// Fyne's own Windows folder-picker sidebar hard-codes the naive path (see
// dialog/file_windows.go's getFavoriteLocation) and silently drops the
// entry when it doesn't exist, rather than resolving the redirect — so a
// user with a redirected Desktop has no sidebar shortcut to it at all.
// knownFolderPath (implemented per-platform in known_folders_windows.go /
// known_folders_other.go) resolves the real path directly via Windows'
// own API instead of guessing, so callers can seed the dialog's starting
// location correctly regardless of Known Folder Move.
type knownFolder int

const (
	knownFolderDesktop knownFolder = iota
	knownFolderDocuments
)
