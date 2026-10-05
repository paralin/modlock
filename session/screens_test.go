package session

import (
	"bufio"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/paralin/modlock/proto/modlock/control"
	"github.com/paralin/modlock/proto/modlock/ui"
)

// relayOrigin is the relay page's origin in these tests.
const relayOrigin = "https://relay.example"

// TestScreensServeTheLocalPlayer streams only the local player's screen to the
// relay page and follows its changes.
func TestScreensServeTheLocalPlayer(t *testing.T) {
	screens := NewScreens(76561198000000001, relayOrigin)
	server := httptest.NewServer(screens)
	defer server.Close()

	// Another player joins and is shown a panel; the local player is not.
	screens.Handle(joined(0, 76561198000000002))
	screens.Handle(shown(0, "arena", "Other"))
	screens.Handle(joined(1, 76561198000000001))

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

	// A change to the local player's interface arrives as the next screen.
	screens.Handle(shown(1, "arena", "Ready"))
	screen := next(t, lines)
	if len(screen.GetTrees()) != 1 || screen.GetTrees()[0].GetMod() != "arena" ||
		screen.GetTrees()[0].GetNodes()[1].GetText() != "Ready" {
		t.Fatalf("unexpected screen %v", screen)
	}
}

// TestScreensRefuseOtherPages refuses pages that are not the game's relay.
func TestScreensRefuseOtherPages(t *testing.T) {
	screens := NewScreens(0, relayOrigin)
	server := httptest.NewServer(screens)
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
	t.Helper()
	if !lines.Scan() {
		t.Fatalf("the stream ended: %v", lines.Err())
	}
	screen := &ui.Screen{}
	if err := screen.UnmarshalJSON(lines.Bytes()); err != nil {
		t.Fatal(err)
	}
	return screen
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
