// Package files keeps one mod's files as modlock-host does: the files the
// mod wrote, in a directory of its own, over the files its build ships in
// data/.
package files

import (
	"io/fs"
	"os"
	"path"
	"strings"

	"github.com/pkg/errors"
)

// Quota bounds the bytes one mod's written files hold together.
const Quota = 4 << 20

// Mod is one mod's files.
type Mod struct {
	// Written is the directory that keeps the files the mod writes, or empty
	// to refuse its writes.
	Written string
	// Data holds the files the build ships, or is nil for none.
	Data fs.FS
}

// Read returns the text of the file name, from the written files when they
// hold one and from the build's otherwise, and false when neither does.
func (m *Mod) Read(name string) (string, bool, error) {
	// Refuse a name that is not a plain relative path.
	if err := Name(name); err != nil {
		return "", false, err
	}

	// Look in the written files first, then in the build's.
	if m.Written != "" {
		root, err := os.OpenRoot(m.Written)
		if err == nil {
			data, err := fs.ReadFile(root.FS(), name)
			_ = root.Close()
			if err == nil {
				return string(data), true, nil
			}
		}
	}
	if m.Data != nil {
		if data, err := fs.ReadFile(m.Data, name); err == nil {
			return string(data), true, nil
		}
	}
	return "", false, nil
}

// Write replaces the file name in the written files with text, whole or not
// at all, while they stay within Quota.
func (m *Mod) Write(name, text string) error {
	// Refuse a write kept nowhere, or a name that is not a plain relative path.
	if m.Written == "" {
		return errors.New("this host keeps no mod files")
	}
	if err := Name(name); err != nil {
		return err
	}
	if err := os.MkdirAll(m.Written, 0o755); err != nil {
		return err
	}
	root, err := os.OpenRoot(m.Written)
	if err != nil {
		return err
	}
	defer root.Close()

	// Refuse a write the quota cannot hold.
	used, err := usage(root.FS(), name)
	if err != nil {
		return err
	}
	if used+int64(len(text)) > Quota {
		return errors.New("the mod's files would pass their 4 MiB quota")
	}

	// Write a temporary file beside the target and rename it into place.
	if dir := path.Dir(name); dir != "." {
		if err := root.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}
	temporary := name + ".writing"
	if err := root.WriteFile(temporary, []byte(text), 0o644); err != nil {
		_ = root.Remove(temporary)
		return err
	}
	if err := root.Rename(temporary, name); err != nil {
		_ = root.Remove(temporary)
		return err
	}
	return nil
}

// Name refuses a name that is not "/"-separated segments without "." or
// "..", empty segments, control characters or characters another platform
// reads as a separator or a drive, so that the name stays inside the
// directory it is joined to on every platform.
func Name(name string) error {
	if !fs.ValidPath(name) || name == "." || strings.ContainsAny(name, "\\:") ||
		strings.ContainsFunc(name, func(r rune) bool { return r < 0x20 }) {
		return errors.Errorf("not a file name: %s", name)
	}
	return nil
}

// usage returns the bytes the files in dir hold, apart from skip.
func usage(dir fs.FS, skip string) (int64, error) {
	var used int64
	err := fs.WalkDir(dir, ".", func(name string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() || name == skip {
			return err
		}
		info, err := entry.Info()
		if err == nil {
			used += info.Size()
		}
		return err
	})
	return used, err
}
