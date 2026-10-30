// Package preview serves the page modlock dev opens beside a running mod: it
// draws every screen the mods show the local player with the game's own
// drawing on a browser stand-in for Panorama, says what the build and the mods
// are doing, and passes the page's presses and chat commands to the mods as
// that player's input. It serves the built mod's layouts as XML and its style
// sheets as CSS, so the page draws a mod's own layouts and sheets too.
package preview

import (
	"context"
	"io/fs"
	"net/http"
	"os"
	"path"
	"slices"
	"strings"
	"sync"
	"time"

	"github.com/aperturerobotics/go-websocket"
	"github.com/paralin/modlock/check"
	"github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/session"
	"github.com/paralin/modlock/source2/panorama"
)

// logLines is how many of a mod's last log lines a stop carries.
const logLines = 20

// frameGap is the least time between two messages on one socket, so a mod
// that changes its interface every frame sends the page its newest screen
// instead of a queue of them.
const frameGap = 50 * time.Millisecond

// Input passes one of the page's inputs to the mods and returns a line for
// the creator when no mod received it, or empty.
type Input func(ctx context.Context, in *cli.Input) string

// Page follows the command line's events and serves the preview page from a
// directory of its files at /. The page opens a websocket at /socket, which
// carries its state down as JSON cli.Preview messages and the player's input
// up as JSON cli.Input messages. GET /layout/<mod>/<path> answers with the
// source of the mod's built layout at path. It serves loopback hosts and its
// own pages only.
type Page struct {
	// files serves the page's files.
	files http.Handler
	// built is the built mod, whose content addon holds its layouts.
	built fs.FS
	// input passes the page's input to the mods.
	input Input
	// screens keeps the local player's screen.
	screens *session.Screens
	// watch is true when the mod rebuilds on each save.
	watch bool

	// mu guards the fields below.
	mu sync.Mutex
	// logs holds each mod's last log lines since it started.
	logs map[string][]string
	// state is what the build and the mods are doing; its screen stays empty,
	// and Observe replaces it whole.
	state *cli.Preview
	// changed is closed and replaced whenever state changes.
	changed chan struct{}
}

// New returns a page serving the files in dir and the layouts of the mod
// built in built, and passing input to the mods, for a mod that rebuilds on
// each save when watch is true. It shows the first player who joins, the
// stand-in player in the sandbox.
func New(dir, built string, watch bool, input Input) *Page {
	return &Page{
		files:   http.FileServer(http.Dir(dir)),
		built:   os.DirFS(built),
		input:   input,
		screens: session.NewScreens(0),
		watch:   watch,
		logs:    make(map[string][]string),
		state:   &cli.Preview{},
		changed: make(chan struct{}),
	}
}

// Observe follows one of the command line's events: a build that starts or
// ends, a mod that starts or stops, or a change to a player's screen.
func (p *Page) Observe(event *cli.Event) {
	// Keep the player's screen.
	host := event.GetHost()
	if host != nil {
		p.screens.Handle(host)
	}

	// Keep each mod's last log lines, and move to the state the event
	// reports.
	p.mu.Lock()
	defer p.mu.Unlock()
	next := &cli.Preview{}
	switch {
	case host.GetLog() != nil:
		mod := host.GetLog().GetMod()
		p.logs[mod] = append(p.logs[mod], host.GetLog().GetText())
		if len(p.logs[mod]) > logLines {
			p.logs[mod] = p.logs[mod][1:]
		}
		return
	case event.GetBuilding() != nil:
		next.State = &cli.Preview_Building{Building: event.GetBuilding()}
	case event.GetBuilt() != nil && !event.GetBuilt().GetOk():
		next.State = &cli.Preview_Failed{Failed: event.GetBuilt()}
	case event.GetBuilt() != nil:
		next.State = &cli.Preview_Running{Running: &cli.Running{}}
	case host.GetStarted() != nil:
		delete(p.logs, host.GetStarted().GetMod())
		next.State = &cli.Preview_Running{Running: &cli.Running{}}
	case host.GetFailed() != nil:
		failed := host.GetFailed()
		next.State = &cli.Preview_Stopped{Stopped: &cli.Stopped{Failed: failed, Log: slices.Clone(p.logs[failed.GetMod()])}}
	default:
		return
	}
	p.state = next
	close(p.changed)
	p.changed = make(chan struct{})
}

// ServeHTTP serves the page and its socket.
func (p *Page) ServeHTTP(w http.ResponseWriter, request *http.Request) {
	// Serve only this computer's own pages.
	if !session.Loopback(request) || !session.SameOrigin(request) {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	switch {
	case request.URL.Path == "/socket":
		p.connect(w, request)
	case request.Method == http.MethodGet && strings.HasPrefix(request.URL.Path, "/layout/"):
		p.layout(w, strings.TrimPrefix(request.URL.Path, "/layout/"))
	case request.Method == http.MethodGet && strings.HasPrefix(request.URL.Path, "/styles/"):
		p.sheet(w, strings.TrimPrefix(request.URL.Path, "/styles/"))
	case request.Method == http.MethodGet:
		p.files.ServeHTTP(w, request)
	default:
		http.NotFound(w, request)
	}
}

// layout answers with the source of the built layout that name, <mod>/<path>,
// names, read from the mod's content addon on each request so a rebuild
// shows at once.
func (p *Page) layout(w http.ResponseWriter, name string) {
	// Find the layout in the addon of the mod's build; a slug names one mod,
	// and its addon is the slug and a version.
	mod, file, _ := strings.Cut(name, "/")
	compiled, ok := panorama.Compiled(file)
	if !ok || !fs.ValidPath(compiled) || mod == "" || strings.Contains(mod, "/") {
		http.NotFound(w, nil)
		return
	}
	kind := path.Join(check.ContentDir, "layout")
	addons, _ := fs.ReadDir(p.built, kind)
	for _, addon := range addons {
		if !addon.IsDir() || !strings.HasPrefix(addon.Name(), mod+"-") {
			continue
		}

		// Answer with the layout as its source reads.
		data, err := fs.ReadFile(p.built, path.Join(kind, addon.Name(), compiled))
		if err != nil {
			break
		}
		source, err := panorama.Decompile(data)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/xml; charset=utf-8")
		_, _ = w.Write([]byte(source))
		return
	}
	http.Error(w, "the mod's build has no layout "+file, http.StatusNotFound)
}

// sheet answers with the source of the built style sheet that name,
// <addon>/<path>, names, as a loaded layout's include names it.
func (p *Page) sheet(w http.ResponseWriter, name string) {
	// Find the sheet in the build's content.
	compiled, ok := panorama.Compiled(name)
	if !ok || path.Ext(name) != ".css" || !fs.ValidPath(compiled) || !strings.Contains(compiled, "/") {
		http.NotFound(w, nil)
		return
	}
	data, err := fs.ReadFile(p.built, path.Join(check.ContentDir, "styles", compiled))
	if err != nil {
		http.Error(w, "the mod's build has no style sheet "+name, http.StatusNotFound)
		return
	}

	// Answer with the sheet as its source reads.
	source, err := panorama.DecompileStyle(data)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/css; charset=utf-8")
	_, _ = w.Write([]byte(source))
}

// current returns the page's state with the player's screen, and channels
// that close when the state or the screen changes.
func (p *Page) current() (*cli.Preview, <-chan struct{}, <-chan struct{}) {
	// Observe replaces the state whole, so its parts are safe to share.
	p.mu.Lock()
	state, changed := p.state, p.changed
	p.mu.Unlock()
	screen, drawn := p.screens.Screen()
	return &cli.Preview{State: state.State, Screen: screen, Watch: p.watch}, changed, drawn
}

// connect speaks with one page until it goes away: it sends the page's state,
// then each changed state, and passes each input the page sends to the mods,
// answering in the state that follows.
func (p *Page) connect(w http.ResponseWriter, request *http.Request) {
	conn, err := websocket.Accept(w, request, nil)
	if err != nil {
		return
	}
	defer conn.CloseNow()
	ctx, cancel := context.WithCancel(request.Context())
	defer cancel()

	// Pass the page's input to the mods, keeping the answer for its next
	// state, until the page goes away.
	var mu sync.Mutex
	var note string
	noted := make(chan struct{}, 1)
	go func() {
		defer cancel()
		for {
			_, data, err := conn.Read(ctx)
			if err != nil {
				return
			}
			in := &cli.Input{}
			if in.UnmarshalJSON(data) != nil {
				continue
			}
			answer := p.input(ctx, in)
			mu.Lock()
			note = answer
			mu.Unlock()
			select {
			case noted <- struct{}{}:
			default:
			}
		}
	}()

	for {
		// Send the current state with the answer to the last input.
		preview, changed, drawn := p.current()
		mu.Lock()
		preview.Note = note
		mu.Unlock()
		data, err := preview.MarshalJSON()
		if err != nil || conn.Write(ctx, websocket.MessageText, data) != nil {
			return
		}

		// Wait for a change, then let the page draw before the next message. An
		// answer belongs to the state it was given in.
		select {
		case <-changed:
			mu.Lock()
			note = ""
			mu.Unlock()
		case <-drawn:
		case <-noted:
		case <-ctx.Done():
			return
		}
		select {
		case <-time.After(frameGap):
		case <-ctx.Done():
			return
		}
	}
}

var _ http.Handler = (*Page)(nil)
