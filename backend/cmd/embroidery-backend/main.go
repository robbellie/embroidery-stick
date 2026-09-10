// Command embroidery-backend is the headless CLI entry point. It
// deliberately does not import the guiapp package (see
// cmd/embroidery-backend-gui) so this binary stays free of Fyne/CGo and
// keeps cross-compiling cheaply from a single Linux CI runner.
package main

import (
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"

	"embroidery-backend/embroidery"
)

func main() {
	dir := flag.String("dir", ".", "directory with embroidery files")
	port := flag.Int("port", embroidery.DefaultPort, "TCP listen port")
	extConfig := flag.String("extensions", "extensions.conf", "file listing allowed file extensions (one per line, # to comment out)")
	flag.Parse()

	srv, err := embroidery.New(embroidery.Config{
		Dir:           *dir,
		Port:          *port,
		ExtConfigPath: *extConfig,
	})
	if err != nil {
		log.Fatalf("startup: %v", err)
	}
	if err := srv.Start(); err != nil {
		log.Fatalf("start: %v", err)
	}

	// SIGHUP triggers a catalog reload (add/remove files without
	// restart) — same behavior as before this package was extracted.
	hup := make(chan os.Signal, 1)
	signal.Notify(hup, syscall.SIGHUP)

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, os.Interrupt, syscall.SIGTERM)

	for {
		select {
		case <-hup:
			log.Printf("SIGHUP: reloading catalog")
			if err := srv.Reload(); err != nil {
				log.Printf("reload error: %v", err)
			}
		case <-quit:
			srv.Stop()
			return
		}
	}
}
