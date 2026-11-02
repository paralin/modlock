package peernet

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/aperturerobotics/starpc/srpc"
	"github.com/paralin/modlock/session"
	"github.com/paralin/modlock/src/host_app/control"
)

// TestSessionServesContentAndPresence walks a guest through a hosted
// session: it fetches the mods' content whole, a map larger than one part
// included, and its watch reports when the server puts the guest in the game
// and takes them out, before and beside their screen.
func TestSessionServesContentAndPresence(t *testing.T) {
	// A built mod with a map archive spanning parts and a compiled layout.
	mod := t.TempDir()
	archive := bytes.Repeat([]byte("map"), contentPart)
	layout := filepath.Join(mod, "panorama", "layout", "arena-0123abcd", "hud.vxml_c")
	for name, data := range map[string][]byte{
		filepath.Join(mod, "maps", "hl_arena.vpk"): archive,
		layout: []byte("layout"),
	} {
		if err := os.MkdirAll(filepath.Dir(name), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(name, data, 0o644); err != nil {
			t.Fatal(err)
		}
	}

	// A host serving the session over an in-memory pipe.
	const code, guest = 7, 76561198000000002
	screens := session.NewScreens(0)
	mux := srpc.NewMux()
	server := &sessionServer{session: code, mods: []string{mod}, screens: screens, chat: session.NewChat()}
	if err := SRPCRegisterSession(mux, server); err != nil {
		t.Fatal(err)
	}
	host := NewSRPCSessionClient(srpc.NewClient(srpc.NewServerPipe(srpc.NewServer(mux))))
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// A wrong join code gets nothing, and the right one every byte.
	if _, err := FetchContent(ctx, host, code+1, nil); err == nil {
		t.Fatal("a wrong join code fetched the content")
	}
	var received uint64
	files, err := FetchContent(ctx, host, code, func(n uint64) { received = n })
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(files["maps/hl_arena.vpk"].Data, archive) || string(files["panorama/layout/arena-0123abcd/hud.vxml_c"].Data) != "layout" {
		t.Fatalf("the content arrived changed: %v", files)
	}
	if received != uint64(len(archive)+len("layout")) {
		t.Fatalf("progress reached %d bytes", received)
	}
	if addons := session.ContentAddons(files); addons["arena"] != "arena-0123abcd" {
		t.Fatalf("the content names addons %v", addons)
	}

	// The watch reports the guest absent first, then present once the
	// server puts them in, then absent when they leave.
	events, err := host.Watch(ctx, &WatchRequest{Session: code, SteamId: guest})
	if err != nil {
		t.Fatal(err)
	}
	defer events.Close()
	present := func(want bool) {
		t.Helper()
		for {
			event, err := events.Recv()
			if err != nil {
				t.Fatal(err)
			}
			if body, ok := event.GetBody().(*SessionEvent_Present); ok {
				if body.Present != want {
					t.Fatalf("presence is %v, want %v", body.Present, want)
				}
				return
			}
		}
	}
	present(false)
	screens.Handle(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: 1, SteamId: guest}}})
	present(true)
	screens.Handle(&control.HostEvent{Body: &control.HostEvent_Left{Left: &control.PlayerLeft{Slot: 1}}})
	present(false)
}
