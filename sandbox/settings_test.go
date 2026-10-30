package sandbox

import (
	"path/filepath"
	"testing"
	"testing/fstest"

	"github.com/paralin/modlock/settings"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm"
	filespb "github.com/paralin/modlock/src/wasm/files"
	manifestpb "github.com/paralin/modlock/src/wasm/manifest"
	settingspb "github.com/paralin/modlock/src/wasm/settings"
)

// TestKept answers the calls of a mod that keeps state across sessions. The
// player starts at a setting's default, keeps an accepted value, and a
// refused value leaves the kept one. A file reads as the build ships it until
// the mod writes its own, and a name that leaves the mod's files is refused.
func TestKept(t *testing.T) {
	// Declare one choice and ship one file, keeping both across sessions.
	manifest := &manifestpb.Manifest{Slug: "hud", Settings: []*settingspb.Setting{{
		Key:     "layout",
		Label:   "Layout",
		Kind:    settingspb.Setting_KIND_CHOICE,
		Choices: []*settingspb.SettingChoice{{Value: "corner"}, {Value: "center"}},
	}}}
	build := &Build{Manifest: manifest, Data: fstest.MapFS{"maps/areas.yaml": {Data: []byte("shipped")}}}
	dir := t.TempDir()
	path := filepath.Join(dir, "settings.json")
	session := func() *Sandbox {
		file, err := settings.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		return &Sandbox{settings: file, config: Config{Files: filepath.Join(dir, "files"), Events: func(*control.HostEvent) {}}}
	}
	read := func(s *Sandbox) string {
		request, _ := (&settingspb.PlayerSettingRequest{Key: "layout"}).MarshalVT()
		response := &settingspb.SettingResponse{}
		if err := response.UnmarshalVT(s.answer(build, &wasm.Call{Method: "PlayerSetting", Request: request}).GetResponse()); err != nil {
			t.Fatal(err)
		}
		return response.GetValue()
	}
	set := func(s *Sandbox, value string) {
		request, _ := (&settingspb.SetPlayerSettingRequest{Key: "layout", Value: value}).MarshalVT()
		s.answer(build, &wasm.Call{Method: "SetPlayerSetting", Request: request})
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

	// Read the shipped file, then the written one in the next session.
	readFile := func(s *Sandbox, name string) (*filespb.ReadFileResponse, string) {
		request, _ := (&filespb.ReadFileRequest{Path: name}).MarshalVT()
		reply := s.answer(build, &wasm.Call{Method: "ReadFile", Request: request})
		response := &filespb.ReadFileResponse{}
		if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
			t.Fatal(err)
		}
		return response, reply.GetError()
	}
	writeFile := func(s *Sandbox, name, text string) string {
		request, _ := (&filespb.WriteFileRequest{Path: name, Text: text}).MarshalVT()
		return s.answer(build, &wasm.Call{Method: "WriteFile", Request: request}).GetError()
	}
	if got, _ := readFile(first, "maps/areas.yaml"); got.GetText() != "shipped" {
		t.Fatalf("read %q, want the shipped file", got.GetText())
	}
	if err := writeFile(first, "maps/areas.yaml", "written"); err != "" {
		t.Fatal(err)
	}
	if got, _ := readFile(session(), "maps/areas.yaml"); got.GetText() != "written" {
		t.Fatalf("the next session read %q, want the written file", got.GetText())
	}

	// A missing file has no text, and a name outside the mod's files is refused.
	if got, err := readFile(first, "missing.yaml"); err != "" || got.Text != nil {
		t.Fatalf("a missing file read %v, %q", got.Text, err)
	}
	if err := writeFile(first, "../hud2/areas.yaml", "x"); err == "" {
		t.Fatal("a write outside the mod's files was accepted")
	}
}
