// Package guiapp is the cross-platform (Linux/Mac/Windows) desktop GUI for
// the embroidery-stick backend. It imports only embroidery-backend/embroidery
// (never its own internals from the reverse direction) and Fyne — Fyne's
// own widgets and dialogs abstract away nearly all platform differences.
// The one deliberate exception is known_folders_windows.go/_other.go:
// Fyne's Windows folder dialog hard-codes %USERPROFILE%\Desktop for its
// sidebar shortcut and silently drops the entry when that's wrong (e.g.
// OneDrive's "Back up your folders" redirects Desktop elsewhere) — see
// known_folders.go for the real fix.
package guiapp

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/storage"
	"fyne.io/fyne/v2/widget"

	"embroidery-backend/embroidery"
)

const (
	prefKeyDir  = "servedDir"
	prefKeyPort = "port"

	// Relative to the working directory, matching the CLI's own default
	// (embroidery.Config.ExtConfigPath) — same file either way, so the
	// GUI and CLI never disagree about which formats are served.
	extConfigPath = "extensions.conf"
)

type guiApp struct {
	fyneApp fyne.App
	win     fyne.Window

	dirEntry    *widget.Label
	portEntry   *widget.Entry
	browseBtn   *widget.Button
	oneDriveBtn *widget.Button // nil if no OneDrive root was found at startup
	extBtn      *widget.Button
	startBtn    *widget.Button

	statusLabel *widget.Label
	activity    *activityDot
	logView     *widget.RichText

	srv *embroidery.Server
}

// Run builds and shows the GUI's single window, blocking until it's closed.
// Called directly from cmd/embroidery-backend-gui's main().
func Run() {
	g := &guiApp{fyneApp: app.NewWithID("com.embroiderystick.backend")}
	g.win = g.fyneApp.NewWindow("Embroidery Stick Backend")

	prefs := g.fyneApp.Preferences()
	// A Label (not an Entry) specifically so a very long path truncates
	// with an ellipsis instead of forcing the whole window wider — an
	// Entry's minimum size grows to fit its full text, which made the
	// window unusably wide for a deeply nested folder path.
	g.dirEntry = widget.NewLabel(prefs.String(prefKeyDir))
	g.dirEntry.Truncation = fyne.TextTruncateEllipsis

	g.portEntry = widget.NewEntry()
	g.portEntry.SetText(strconv.Itoa(prefs.IntWithFallback(prefKeyPort, embroidery.DefaultPort)))

	g.browseBtn = widget.NewButton("Browse...", g.onBrowse)
	g.extBtn = widget.NewButton("File types...", g.onEditExtensions)
	g.startBtn = widget.NewButton("Start", g.onToggleStartStop)

	// Only shown if a OneDrive root actually resolves (mainly a Windows
	// concern — see known_folders.go): a quick way into a OneDrive tree
	// without depending on Fyne's own Desktop/Documents sidebar shortcuts,
	// which silently disappear under Known Folder Move.
	if root, ok := oneDriveRoot(); ok {
		g.oneDriveBtn = widget.NewButton("OneDrive...", func() { g.onBrowseFrom(root) })
	}

	g.statusLabel = widget.NewLabel("Idle")
	g.activity = newActivityDot()

	// RichText, not a disabled Entry: a disabled Entry renders its text in
	// the theme's muted/greyed color (meant for "this input is
	// unavailable"), which made the log unreadable — RichText is a genuine
	// read-only display widget, so it renders with normal text color.
	g.logView = widget.NewRichTextWithText("")
	g.logView.Wrapping = fyne.TextWrapWord

	browseButtons := fyne.CanvasObject(g.browseBtn)
	if g.oneDriveBtn != nil {
		browseButtons = container.NewHBox(g.oneDriveBtn, g.browseBtn)
	}
	form := container.NewBorder(nil, nil, widget.NewLabel("Folder:"), browseButtons, g.dirEntry)
	portRow := container.NewBorder(nil, nil, widget.NewLabel("Port:"), g.extBtn, g.portEntry)
	statusRow := container.NewBorder(nil, nil, g.activity.canvasObject(), nil, g.statusLabel)

	content := container.NewBorder(
		container.NewVBox(form, portRow, g.startBtn, widget.NewSeparator(), statusRow),
		nil, nil, nil,
		container.NewScroll(g.logView),
	)

	g.win.SetContent(content)
	g.win.Resize(fyne.NewSize(520, 420))
	g.win.SetCloseIntercept(func() {
		if g.srv != nil {
			g.srv.Stop()
		}
		g.win.Close()
	})
	g.win.ShowAndRun()
}

// onBrowse opens the folder picker. Where it starts: the previously
// chosen folder if one is already set (so re-browsing doesn't lose your
// place), otherwise the real Desktop path — resolved via
// known_folders.go rather than trusting Fyne's own Windows sidebar
// shortcut, which is hard-coded to %USERPROFILE%\Desktop and silently
// disappears once OneDrive folder redirection (Known Folder Move) points
// Desktop elsewhere. Falls through to Fyne's own default location if
// neither resolves (e.g. non-Windows, or first run with nothing set).
func (g *guiApp) onBrowse() {
	start := g.dirEntry.Text
	if start == "" {
		start, _ = knownFolderPath(knownFolderDesktop)
	}
	g.onBrowseFrom(start)
}

// onBrowseFrom opens the folder picker starting at startPath (empty
// falls through to Fyne's own default). Shared by onBrowse and the
// OneDrive quick-access button.
func (g *guiApp) onBrowseFrom(startPath string) {
	d := dialog.NewFolderOpen(func(uri fyne.ListableURI, err error) {
		if err != nil || uri == nil {
			return
		}
		g.dirEntry.SetText(uri.Path())
		g.fyneApp.Preferences().SetString(prefKeyDir, uri.Path())
	}, g.win)
	if startPath != "" {
		if lister, err := storage.ListerForURI(storage.NewFileURI(startPath)); err == nil {
			d.SetLocation(lister)
		}
	}
	d.Show()
}

// oneDriveRoot finds the local OneDrive sync root via the environment
// variables Windows' OneDrive client sets: OneDrive is the general one
// (present for either account type), OneDriveCommercial/OneDriveConsumer
// distinguish a work-or-school vs. personal account when both are signed
// in on the same machine. First one that's actually set and exists wins.
// ok is false if none apply (not Windows, OneDrive not installed or not
// signed in).
func oneDriveRoot() (string, bool) {
	for _, envVar := range []string{"OneDrive", "OneDriveCommercial", "OneDriveConsumer"} {
		p := os.Getenv(envVar)
		if p == "" {
			continue
		}
		if info, err := os.Stat(p); err == nil && info.IsDir() {
			return p, true
		}
	}
	return "", false
}

// onEditExtensions lets the user pick which embroidery file formats are
// served via checkboxes, instead of hand-editing extensions.conf — which
// doesn't fit this project's "no hassle for the end user" goal. Reads and
// writes the same file the CLI uses, so both stay in sync regardless of
// which one last touched it. Left enabled even while the server is
// running (unlike Browse/port) since changing the extension filter is
// safe to apply live via Server.SetAllowedExtensions — no restart needed.
func (g *guiApp) onEditExtensions() {
	allowed, err := embroidery.LoadAllowedExtensions(extConfigPath)
	if err != nil {
		dialog.ShowError(err, g.win)
		return
	}

	checks := make(map[string]*widget.Check, len(embroidery.KnownExtensions))
	box := container.NewVBox()
	for _, ext := range embroidery.KnownExtensions {
		chk := widget.NewCheck(strings.TrimPrefix(ext, "."), nil)
		chk.SetChecked(allowed[ext])
		checks[ext] = chk
		box.Add(chk)
	}

	dialog.ShowCustomConfirm("File types to serve", "Apply", "Cancel", box, func(ok bool) {
		if !ok {
			return
		}
		newAllowed := make(map[string]bool, len(checks))
		for ext, chk := range checks {
			newAllowed[ext] = chk.Checked
		}
		if err := embroidery.SaveAllowedExtensions(extConfigPath, newAllowed); err != nil {
			dialog.ShowError(err, g.win)
			return
		}
		if g.srv != nil {
			if err := g.srv.SetAllowedExtensions(newAllowed); err != nil {
				dialog.ShowError(err, g.win)
				return
			}
			g.appendLog("File type filter updated")
		}
	}, g.win)
}

func (g *guiApp) appendLog(msg string) {
	fyne.Do(func() {
		// Appends a new segment instead of rebuilding the whole log text
		// (String() + re-wrap into one segment) on every single line — that
		// was O(total log length) per call, so it got measurably slower as
		// a session went on, entirely avoidable since RichText already
		// supports multiple segments natively.
		stamp := time.Now().Format("15:04:05")
		g.logView.Segments = append(g.logView.Segments, &widget.TextSegment{
			Style: widget.RichTextStyleInline,
			Text:  fmt.Sprintf("[%s] %s\n", stamp, msg),
		})
		g.logView.Refresh()
	})
}

func (g *guiApp) setStatus(text string) {
	fyne.Do(func() { g.statusLabel.SetText(text) })
}

func (g *guiApp) setFieldsEnabled(enabled bool) {
	if enabled {
		g.browseBtn.Enable()
		g.portEntry.Enable()
		if g.oneDriveBtn != nil {
			g.oneDriveBtn.Enable()
		}
	} else {
		g.browseBtn.Disable()
		g.portEntry.Disable()
		if g.oneDriveBtn != nil {
			g.oneDriveBtn.Disable()
		}
	}
}

func (g *guiApp) onToggleStartStop() {
	if g.srv != nil {
		g.stopServer()
		return
	}
	g.startServer()
}

func (g *guiApp) startServer() {
	dir := g.dirEntry.Text
	if dir == "" {
		dialog.ShowInformation("No folder selected", "Choose a folder to serve first.", g.win)
		return
	}
	port, err := strconv.Atoi(g.portEntry.Text)
	if err != nil || port <= 0 || port > 65535 {
		dialog.ShowInformation("Invalid port", "Port must be a number between 1 and 65535.", g.win)
		return
	}
	g.fyneApp.Preferences().SetInt(prefKeyPort, port)

	srv, err := embroidery.New(embroidery.Config{
		Dir:           dir,
		Port:          port,
		ExtConfigPath: extConfigPath,
		OnFileRead: func(path string, offset, length uint32) {
			g.activity.flash()
			g.setStatus(fmt.Sprintf("Reading %s...", path))
		},
		OnFileDetected: func(path string, added bool) {
			if added {
				g.appendLog(fmt.Sprintf("New file detected: %s", path))
			} else {
				g.appendLog(fmt.Sprintf("File removed: %s", path))
			}
		},
		OnLog: g.appendLog,
	})
	if err != nil {
		dialog.ShowError(err, g.win)
		return
	}
	if err := srv.Start(); err != nil {
		dialog.ShowError(err, g.win)
		return
	}

	g.srv = srv
	g.setFieldsEnabled(false)
	g.startBtn.SetText("Stop")
	g.setStatus(fmt.Sprintf("Listening on %s, serving %s", srv.Addr(), dir))
}

func (g *guiApp) stopServer() {
	g.srv.Stop()
	g.srv = nil
	g.setFieldsEnabled(true)
	g.startBtn.SetText("Start")
	g.setStatus("Idle")
}
