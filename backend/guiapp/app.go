// Package guiapp is the cross-platform (Linux/Mac/Windows) desktop GUI for
// the embroidery-stick backend. It imports only embroidery-backend/embroidery
// (never its own internals from the reverse direction) and Fyne — no
// platform-specific code, since Fyne's own widgets and dialogs already
// abstract that away.
package guiapp

import (
	"fmt"
	"strconv"
	"strings"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
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

	dirEntry  *widget.Label
	portEntry *widget.Entry
	browseBtn *widget.Button
	extBtn    *widget.Button
	startBtn  *widget.Button

	statusLabel *widget.Label
	activity    *activityDot
	logView     *widget.Entry

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

	g.statusLabel = widget.NewLabel("Idle")
	g.activity = newActivityDot()

	g.logView = widget.NewMultiLineEntry()
	g.logView.Wrapping = fyne.TextWrapWord
	g.logView.Disable() // read-only log pane; Fyne has no dedicated read-only text widget

	form := container.NewBorder(nil, nil, widget.NewLabel("Folder:"), g.browseBtn, g.dirEntry)
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

func (g *guiApp) onBrowse() {
	dialog.NewFolderOpen(func(uri fyne.ListableURI, err error) {
		if err != nil || uri == nil {
			return
		}
		g.dirEntry.SetText(uri.Path())
		g.fyneApp.Preferences().SetString(prefKeyDir, uri.Path())
	}, g.win).Show()
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
		stamp := time.Now().Format("15:04:05")
		g.logView.SetText(g.logView.Text + fmt.Sprintf("[%s] %s\n", stamp, msg))
	})
}

func (g *guiApp) setStatus(text string) {
	fyne.Do(func() { g.statusLabel.SetText(text) })
}

func (g *guiApp) setFieldsEnabled(enabled bool) {
	if enabled {
		g.browseBtn.Enable()
		g.portEntry.Enable()
	} else {
		g.browseBtn.Disable()
		g.portEntry.Disable()
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
