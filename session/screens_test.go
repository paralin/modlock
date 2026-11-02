package session

import (
	"bufio"
	"maps"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"testing/fstest"

	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm/ui"
)

// relayOrigin is the relay page's origin in these tests.
const relayOrigin = "https://relay.example"

// TestScreensServeEachPlayer streams the local player's screen to the relay
// page, and carries another player's screen to a mirror as changes, as a host
// does for a guest.
func TestScreensServeEachPlayer(t *testing.T) {
	// Another player joins and is shown a panel; the local player is not.
	const local, guest = 76561198000000001, 76561198000000002
	screens := NewScreens(local)
	server := httptest.NewServer(NewRelay(relayOrigin, screens.Screen))
	defer server.Close()
	screens.Handle(joined(0, guest))
	screens.Handle(shown(0, "arena", "Other"))
	screens.Handle(joined(1, local))

	// The stream starts with the local player's empty screen.
	request := relayRequest(t, server.URL+"/panorama/stream")
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	if response.Header.Get("Access-Control-Allow-Origin") != relayOrigin {
		t.Fatalf("the relay was not admitted: %v", response.Header)
	}
	lines := bufio.NewScanner(response.Body)
	if first := next(t, lines); len(first.GetTrees()) != 0 {
		t.Fatalf("the local player saw another player's screen: %v", first)
	}

	// A change to the local player's interface arrives as the next screen,
	// and the other player's change sends no frame.
	screens.Handle(shown(0, "arena", "Still other"))
	screens.Handle(shown(1, "arena", "Ready"))
	if screen := next(t, lines); label(screen) != "Ready" {
		t.Fatalf("unexpected screen %v", screen)
	}

	// The other player's screen reaches a mirror whole.
	mirror := NewMirror()
	sent, present, _ := screens.Player(guest)
	if !present {
		t.Fatal("a joined guest is not in the server")
	}
	mirror.Apply(Changes(&ui.Screen{}, sent), true)

	// A later change reaches it as the one node that changed.
	screens.Handle(shown(0, "arena", "Go"))
	now, _, _ := screens.Player(guest)
	changes := Changes(sent, now)
	if len(changes) != 1 || changes["arena"].GetReset_() || len(changes["arena"].GetSet()) != 1 {
		t.Fatalf("the change was not one node: %v", changes)
	}
	mirror.Apply(changes, false)
	if screen, _ := mirror.Screen(); label(screen) != "Go" || !screen.EqualVT(now) {
		t.Fatalf("the mirror shows %v", screen)
	}

	// A reload clears and redraws before the guest samples. Identical nodes
	// still start another lifetime, carried through a daemon snapshot and diff.
	screens.Handle(&control.HostEvent{Body: &control.HostEvent_Ui{Ui: &control.UiChanged{Mod: "arena", Change: &ui.Change{Reset_: true}}}})
	screens.Handle(shown(0, "arena", "Go"))
	relay := NewScreens(0)
	for _, event := range screens.Snapshot() {
		relay.Handle(event)
	}

	// The guest's sampled delta preserves the lifetime, even though its
	// previous frame and the replacement contain the same nodes.
	reloaded, _, _ := relay.Player(guest)
	changes = Changes(now, reloaded)
	if !changes["arena"].GetReset_() || changes["arena"].GetGeneration() != now.GetTrees()[0].GetGeneration()+1 {
		t.Fatalf("the reload lost its lifetime: %v", changes)
	}
	mirror.Apply(changes, false)
	if screen, _ := mirror.Screen(); !screen.EqualVT(reloaded) {
		t.Fatalf("the mirrored reload differs: %v", screen)
	}
	now = reloaded

	// A new owner starts another lifetime even with the same mod and nodes.
	restarted := NewScreens(0)
	restarted.Handle(joined(0, guest))
	restarted.Handle(shown(0, "arena", "Go"))
	fresh, _, _ := restarted.Player(guest)
	if !Changes(sent, fresh)["arena"].GetReset_() {
		t.Fatal("the restarted owner reused its initial lifetime")
	}
	changes = Changes(now, fresh)
	if !changes["arena"].GetReset_() || changes["arena"].GetGeneration() == now.GetTrees()[0].GetGeneration() {
		t.Fatalf("the restarted owner reused its lifetime: %v", changes)
	}

	// Mirroring the new owner preserves its stamp through the next relay.
	mirror.Apply(changes, false)
	if screen, _ := mirror.Screen(); !screen.EqualVT(fresh) {
		t.Fatalf("the restarted screen differs: %v", screen)
	}
	now = fresh

	// A tree the mod hides is cleared on the mirror, and a player who
	// leaves has an empty screen.
	screens.Handle(&control.HostEvent{Body: &control.HostEvent_Ui{Ui: &control.UiChanged{Mod: "arena", Change: &ui.Change{Reset_: true}}}})
	gone, _, _ := screens.Player(guest)
	mirror.Apply(Changes(now, gone), false)
	if screen, _ := mirror.Screen(); len(screen.GetTrees()) != 0 {
		t.Fatalf("the hidden tree stayed: %v", screen)
	}
	screens.Handle(&control.HostEvent{Body: &control.HostEvent_Left{Left: &control.PlayerLeft{Slot: 1}}})
	if screen, present, _ := screens.Player(local); present || len(screen.GetTrees()) != 0 {
		t.Fatalf("a player who left kept a screen: %v, present %v", screen, present)
	}
}

// TestScreensRefuseOtherPages refuses pages that are not the game's relay.
func TestScreensRefuseOtherPages(t *testing.T) {
	server := httptest.NewServer(NewRelay(relayOrigin, NewMirror().Screen))
	defer server.Close()
	for name, header := range map[string]http.Header{
		"another site":      {"Origin": {"https://other.example"}, "User-Agent": {RelayAgent}},
		"a browser":         {"Origin": {relayOrigin}, "User-Agent": {"Mozilla/5.0"}},
		"a rebound address": {"Host": {"attacker.example"}},
	} {
		request, err := http.NewRequest(http.MethodGet, server.URL+"/panorama/stream", nil)
		if err != nil {
			t.Fatal(err)
		}
		request.Header = header
		if host := header.Get("Host"); host != "" {
			request.Host = host
		}
		response, err := http.DefaultClient.Do(request)
		if err != nil {
			t.Fatal(err)
		}
		response.Body.Close()
		if response.StatusCode != http.StatusForbidden {
			t.Errorf("%s was answered with %d", name, response.StatusCode)
		}
	}
}

// relayRequest returns a request as the game's relay page sends it.
func relayRequest(t *testing.T, url string) *http.Request {
	// Name the relay's origin and the game's panel.
	t.Helper()
	request, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Origin", relayOrigin)
	request.Header.Set("User-Agent", "Mozilla/5.0 Valve Source2 HTML/1.0")
	return request
}

// next reads the next screen from the stream.
func next(t *testing.T, lines *bufio.Scanner) *ui.Screen {
	// Read one packed line.
	t.Helper()
	if !lines.Scan() {
		t.Fatalf("the stream ended: %v", lines.Err())
	}
	sequence, packed, ok := strings.Cut(strings.TrimPrefix(lines.Text(), "~"), ":")
	if !ok || sequence == "" {
		t.Fatalf("not a packed line: %q", lines.Text())
	}

	// Unpack it into a screen.
	var frame []byte
	for _, char := range packed {
		if char >= 0x100 {
			char -= 0x100
		}
		frame = append(frame, byte(char))
	}
	screen := &ui.Screen{}
	if err := screen.UnmarshalVT(frame); err != nil {
		t.Fatal(err)
	}
	return screen
}

// label returns the text of the button the arena mod shows, if any.
func label(screen *ui.Screen) string {
	for _, tree := range screen.GetTrees() {
		for _, node := range tree.GetNodes() {
			if tree.GetMod() == "arena" && node.GetId() == "b" {
				return node.GetText()
			}
		}
	}
	return ""
}

// joined is the event of a person joining in slot.
func joined(slot int32, account uint64) *control.HostEvent {
	return &control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{Slot: slot, SteamId: account}}}
}

// shown is the event of mod showing the player in slot a button labeled text.
func shown(slot int32, mod, text string) *control.HostEvent {
	change := &ui.Change{Reset_: true, Set: []*ui.Node{
		{Id: "", Children: []string{"b"}},
		{Id: "b", Kind: ui.Kind_KIND_BUTTON, Text: text},
	}}
	return &control.HostEvent{Body: &control.HostEvent_Ui{Ui: &control.UiChanged{Mod: mod, Slot: slot, Change: change}}}
}

// TestServedNamesContentAddons shows the renderer which content addon holds
// each mod's layouts: the addon its content names, the rebuilt one after a
// reload, and none for a mod with no layouts.
func TestServedNamesContentAddons(t *testing.T) {
	// A screen with two mods' trees, one of which ships layouts.
	screens := NewScreens(0)
	screens.Handle(joined(0, 76561198000000001))
	screens.Handle(shown(0, "arena", "Ready"))
	screens.Handle(shown(0, "plain", "Ready"))
	content := fstest.MapFS{
		"panorama/layout/arena-0123abcd/shop.vxml_c": {Data: []byte("layout")},
		"panorama/styles/other-89abcdef/hud.vcss_c":  {Data: []byte("sheet")},
		"panorama/layout/not-an-addon/shop.vxml_c":   {Data: []byte("layout")},
		"panorama/scripts/arena-0123abcd/shop.vjs_c": {Data: []byte("script")},
	}
	served := &Served{config: Interface{Screen: screens.Screen}, addons: ContentAddons(content)}
	addons := func() map[string]string {
		screen, _ := served.screen()
		named := make(map[string]string)
		for _, tree := range screen.GetTrees() {
			named[tree.GetMod()] = tree.GetAddon()
		}
		return named
	}

	// The mod with layouts names its addon; the other names none.
	if got := addons(); got["arena"] != "arena-0123abcd" || got["plain"] != "" || len(got) != 2 {
		t.Fatalf("addons of the screen's trees: %v", got)
	}

	// A rebuild's content takes over the mod's addon.
	served.mu.Lock()
	maps.Copy(served.addons, ContentAddons(fstest.MapFS{"panorama/layout/arena-4567cdef/shop.vxml_c": {}}))
	served.mu.Unlock()
	if got := addons(); got["arena"] != "arena-4567cdef" {
		t.Fatalf("addon after a rebuild: %v", got)
	}
}
