// Package studio serves Modlock Studio: one web page, built with the Modlock
// library, whose first workspace browses and draws the game's own assets from
// the player's install. Nothing of the game ships with Modlock; the page
// reads every asset through the local server from the files on disk.
package studio

import (
	"net/http"
	"os"
)

// Handler returns the page in the directory page and the API of every
// workspace. Without assets, the page still serves and says why it has no
// archive to show.
func Handler(page string, assets *Assets) http.Handler {
	mux := http.NewServeMux()
	if assets != nil {
		assets.routes(mux)
	}
	mux.Handle("GET /", http.FileServerFS(os.DirFS(page)))
	return mux
}
