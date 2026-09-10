package guiapp

import (
	"image/color"
	"sync"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/container"
)

const flashDuration = 400 * time.Millisecond

var (
	colorIdle   = color.NRGBA{R: 0x9e, G: 0x9e, B: 0x9e, A: 0xff}
	colorActive = color.NRGBA{R: 0x2e, G: 0xa0, B: 0x4a, A: 0xff}
)

// activityDot is a small colored indicator that flashes green briefly on
// each OnFileRead callback, so "currently reading" is visually distinct
// from "idle listening" rather than just another line scrolling past in
// the log. flash() is called from arbitrary goroutines (the server's
// per-connection handler) and is safe for concurrent use.
type activityDot struct {
	circle *canvas.Circle

	mu    sync.Mutex
	timer *time.Timer
}

func newActivityDot() *activityDot {
	return &activityDot{circle: canvas.NewCircle(colorIdle)}
}

// canvasObject returns the dot fixed at a small, constant size regardless
// of the surrounding layout (canvas.Circle's own MinSize is {1,1}).
func (a *activityDot) canvasObject() fyne.CanvasObject {
	return container.NewGridWrap(fyne.NewSize(14, 14), a.circle)
}

func (a *activityDot) flash() {
	fyne.Do(func() {
		a.circle.FillColor = colorActive
		a.circle.Refresh()
	})

	a.mu.Lock()
	defer a.mu.Unlock()
	if a.timer != nil {
		a.timer.Stop()
	}
	a.timer = time.AfterFunc(flashDuration, func() {
		fyne.Do(func() {
			a.circle.FillColor = colorIdle
			a.circle.Refresh()
		})
	})
}
