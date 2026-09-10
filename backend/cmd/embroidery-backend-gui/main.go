// Command embroidery-backend-gui is the desktop GUI entry point. Requires
// CGo and a platform GUI toolchain (see guiapp's Fyne dependency) — kept in
// its own binary, separate from cmd/embroidery-backend, so the headless
// CLI build stays free of that requirement.
package main

import "embroidery-backend/guiapp"

func main() {
	guiapp.Run()
}
