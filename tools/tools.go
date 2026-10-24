// Package tools serves Modlock's tools page: one web app, embedded in the
// command line, whose first tool browses and renders the game's own assets
// from the player's install. Nothing of the game ships with Modlock; the
// page reads every asset through the local server from the files on disk.
package tools

import (
	"embed"
	"io/fs"
	"net/http"
)

// web holds the built page: index.html and its bundle.
//
//go:embed all:web/dist
var web embed.FS

// Handler returns the page and the API of every tool.
func Handler(assets *Assets) http.Handler {
	mux := http.NewServeMux()
	if assets != nil {
		assets.routes(mux)
	}
	dist, _ := fs.Sub(web, "web/dist")
	mux.Handle("GET /", http.FileServerFS(dist))
	return mux
}
