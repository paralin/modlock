package project

import (
	"context"
	"io/fs"
	"path/filepath"
	"slices"
	"strings"
	"time"

	"github.com/fsnotify/fsnotify"
	"github.com/pkg/errors"
)

// settle is how long the sources stay unchanged before Watch reports them;
// an editor's save writes several events.
const settle = 150 * time.Millisecond

// Watch calls changed after the project's sources change, once per burst of
// changes, until ctx ends. It skips the build directory, the installed
// library and hidden directories.
func (p *Project) Watch(ctx context.Context, changed func()) error {
	// Watch every source directory; fsnotify watches one level each.
	watcher, err := fsnotify.NewWatcher()
	if err != nil {
		return errors.Wrap(err, "watch the project")
	}
	defer watcher.Close()
	add := func(root string) error {
		return filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
			if err != nil || !entry.IsDir() {
				return err
			}
			if p.skip(path) {
				return filepath.SkipDir
			}
			return watcher.Add(path)
		})
	}
	if err := add(p.Dir); err != nil {
		return errors.Wrap(err, "watch the project")
	}

	// Report each burst once it settles.
	timer := time.NewTimer(settle)
	timer.Stop()
	for {
		select {
		case <-ctx.Done():
			return context.Canceled
		case err := <-watcher.Errors:
			return errors.Wrap(err, "watch the project")
		case event := <-watcher.Events:
			if p.skip(event.Name) {
				continue
			}
			if event.Has(fsnotify.Create) {
				_ = add(event.Name)
			}
			timer.Reset(settle)
		case <-timer.C:
			changed()
		}
	}
}

// skip reports whether path is outside the sources: the build directory, the
// installed packages, or within a hidden file or directory.
func (p *Project) skip(path string) bool {
	relative, err := filepath.Rel(p.Dir, path)
	if err != nil || relative == "." {
		return false
	}
	parts := strings.Split(relative, string(filepath.Separator))
	return parts[0] == BuildDir || parts[0] == "node_modules" || slices.ContainsFunc(parts, func(part string) bool {
		return strings.HasPrefix(part, ".")
	})
}
