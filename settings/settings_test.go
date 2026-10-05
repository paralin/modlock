package settings

import (
	"path/filepath"
	"strings"
	"testing"

	"github.com/paralin/modlock/proto/modlock/wasm"
)

// TestValidate accepts declarations players can choose from and refuses the
// rest with the reason.
func TestValidate(t *testing.T) {
	layout := func() *wasm.Setting {
		return &wasm.Setting{
			Key:     "layout",
			Label:   "Layout",
			Kind:    wasm.Setting_KIND_CHOICE,
			Choices: []*wasm.SettingChoice{{Value: "corner"}, {Value: "center"}},
		}
	}
	for name, test := range map[string]struct {
		edit func(*wasm.Setting)
		fail string
	}{
		"valid":       {func(*wasm.Setting) {}, ""},
		"default":     {func(s *wasm.Setting) { s.DefaultValue = "center" }, ""},
		"bad default": {func(s *wasm.Setting) { s.DefaultValue = "top" }, "default"},
		"bad key":     {func(s *wasm.Setting) { s.Key = "2d" }, "key"},
		"no label":    {func(s *wasm.Setting) { s.Label = "" }, "label"},
		"no kind":     {func(s *wasm.Setting) { s.Kind = wasm.Setting_KIND_UNKNOWN }, "kind"},
		"twice":       {func(s *wasm.Setting) { s.Choices[1].Value = "corner" }, "twice"},
		"bounds":      {func(s *wasm.Setting) { s.Kind, s.Min, s.Max = wasm.Setting_KIND_NUMBER, 2, 1 }, "min"},
	} {
		setting := layout()
		test.edit(setting)
		err := Validate(&wasm.Manifest{Settings: []*wasm.Setting{setting}})
		if test.fail == "" && err != nil || test.fail != "" && (err == nil || !strings.Contains(err.Error(), test.fail)) {
			t.Errorf("%s: got %v, want %q", name, err, test.fail)
		}
	}
	if err := Validate(&wasm.Manifest{Settings: []*wasm.Setting{layout(), layout()}}); err == nil {
		t.Error("accepted a setting declared twice")
	}
}

// TestFile keeps values across opens.
func TestFile(t *testing.T) {
	// Store two values in turn.
	path := filepath.Join(t.TempDir(), "settings.json")
	file, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, value := range []string{"corner", "center"} {
		if err := file.Store("hud", 7, "layout", value); err != nil {
			t.Fatal(err)
		}
	}

	// A new open reads the last value, for that player only.
	reopened, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if value, ok := reopened.Value("hud", 7, "layout"); value != "center" || !ok {
		t.Fatalf("got %q, %v; want center", value, ok)
	}
	if _, ok := reopened.Value("hud", 8, "layout"); ok {
		t.Fatal("another player read the value")
	}
}
