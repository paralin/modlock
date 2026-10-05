package settings

import (
	"os"
	"path/filepath"

	"github.com/paralin/modlock/proto/modlock/wasm"
)

// File keeps players' settings in memory and, given a path, in a JSON file of
// wasm.StoredSettings, the same file modlock-host keeps. It reads the file
// when opened and rewrites it on each change. A File is not safe for
// concurrent use.
type File struct {
	// path is the file, or empty to keep values in memory only.
	path string
	// stored holds every kept value.
	stored *wasm.StoredSettings
}

// Open reads the settings file at path, or starts empty when path is empty
// or the file does not exist. A file that does not decode starts empty, and
// the error says why.
func Open(path string) (*File, error) {
	// Start empty without a file.
	f := &File{path: path, stored: &wasm.StoredSettings{}}
	if path == "" {
		return f, nil
	}

	// Read the file; one that does not decode starts empty.
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return f, nil
	}
	if err == nil {
		err = f.stored.UnmarshalJSON(data)
	}
	if err != nil {
		f.stored = &wasm.StoredSettings{}
	}
	return f, err
}

// Value returns the value the player chose for the mod's setting and whether
// one is kept.
func (f *File) Value(mod string, steamID uint64, key string) (string, bool) {
	if value := f.find(mod, steamID, key); value != nil {
		return value.GetValue(), true
	}
	return "", false
}

// Store keeps the player's new value of the mod's setting and rewrites the
// file. The value stays in memory when the file cannot be written.
func (f *File) Store(mod string, steamID uint64, key, value string) error {
	// Replace the player's earlier value, or add the first.
	found := f.find(mod, steamID, key)
	if found == nil {
		found = &wasm.StoredSetting{Mod: mod, SteamId: steamID, Key: key}
		f.stored.Values = append(f.stored.Values, found)
	}
	found.Value = value

	// Rewrite the file whole through a staged copy.
	if f.path == "" {
		return nil
	}
	data, err := f.stored.MarshalJSON()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(f.path), 0o755); err != nil {
		return err
	}
	staged := f.path + ".tmp"
	if err := os.WriteFile(staged, data, 0o644); err != nil {
		return err
	}
	return os.Rename(staged, f.path)
}

// find returns the kept value of the mod's setting for the player, or nil.
func (f *File) find(mod string, steamID uint64, key string) *wasm.StoredSetting {
	for _, value := range f.stored.GetValues() {
		if value.GetMod() == mod && value.GetSteamId() == steamID && value.GetKey() == key {
			return value
		}
	}
	return nil
}
