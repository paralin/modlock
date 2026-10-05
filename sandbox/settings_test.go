package sandbox

import (
	"path/filepath"
	"testing"

	"github.com/paralin/modlock/proto/modlock/control"
	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/paralin/modlock/settings"
)

// TestSettings answers a mod's settings calls: the player starts at the
// default, keeps an accepted value across sessions, and a refused value
// leaves the kept one.
func TestSettings(t *testing.T) {
	// Declare one choice and keep it in a file across sessions.
	manifest := &wasm.Manifest{Slug: "hud", Settings: []*wasm.Setting{{
		Key:     "layout",
		Label:   "Layout",
		Kind:    wasm.Setting_KIND_CHOICE,
		Choices: []*wasm.SettingChoice{{Value: "corner"}, {Value: "center"}},
	}}}
	path := filepath.Join(t.TempDir(), "settings.json")
	session := func() *Sandbox {
		file, err := settings.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		return &Sandbox{settings: file, config: Config{Events: func(*control.HostEvent) {}}}
	}
	read := func(s *Sandbox) string {
		request, _ := (&wasm.PlayerSettingRequest{Key: "layout"}).MarshalVT()
		response := &wasm.SettingResponse{}
		if err := response.UnmarshalVT(s.answer(manifest, &wasm.Call{Method: "PlayerSetting", Request: request}).GetResponse()); err != nil {
			t.Fatal(err)
		}
		return response.GetValue()
	}
	set := func(s *Sandbox, value string) {
		request, _ := (&wasm.SetPlayerSettingRequest{Key: "layout", Value: value}).MarshalVT()
		s.answer(manifest, &wasm.Call{Method: "SetPlayerSetting", Request: request})
	}

	// Start at the default, keep the accepted value and refuse the other.
	first := session()
	if got := read(first); got != "corner" {
		t.Fatalf("started at %q, want corner", got)
	}
	set(first, "center")
	set(first, "top")
	if got := read(session()); got != "center" {
		t.Fatalf("the next session read %q, want center", got)
	}
}
