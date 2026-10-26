// Package devmount owns the package a development session mounts in a
// Deadlock installation: its directory, its block in gameinfo.gi, and the lock
// that tells a live session from one that ended without unmounting. Each
// package lives in its own subdirectory named for its content, since a running
// game holds the package it loaded open and Windows refuses to replace it; a
// changed package goes beside the held one, and the next session prunes the
// ones no game holds. Another
// program that mounts content in the same game, such as a game client that
// refuses to start beside conflicting packages, clears a stale mount through
// ClearStale instead of asking the player to edit gameinfo.gi.
package devmount

import (
	"bytes"
	"os"
	"path/filepath"

	"github.com/paralin/modlock/internal/filelock"
	"github.com/paralin/modlock/internal/fileretry"
	"github.com/paralin/modlock/source2/gameinfo"
	"github.com/pkg/errors"
)

const (
	// Name names the session's block in gameinfo.gi.
	Name = "MODLOCK DEV"
	// Dir is the game-relative directory of the session's packages.
	Dir = "citadel/modlock_dev"
	// lockName is the lock file in Dir that each live session holds shared.
	lockName = "session.lock"
)

// ErrRunning reports that a live development session holds the mount.
var ErrRunning = errors.New("a Modlock development session is running in this game")

// Path returns the sessions' directory in the installation at gameDir.
func Path(gameDir string) string {
	return filepath.Join(gameDir, "game", filepath.FromSlash(Dir))
}

// PackagePath returns the directory of the package named key in the
// installation at gameDir.
func PackagePath(gameDir, key string) string {
	return filepath.Join(Path(gameDir), key)
}

// Prune removes every package but keep's while no other session is live, and
// keeps going past an entry it cannot remove, such as a package a running game
// still holds. Prune before Hold, whose shared lock would count as live.
func Prune(gameDir, keep string) error {
	// Prune only while no other session holds the lock.
	dir := Path(gameDir)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	held, err := filelock.Lock(filepath.Join(dir, lockName), true, false)
	if err == filelock.ErrLocked {
		return nil
	}
	if err != nil {
		return err
	}
	defer held.Close()

	// Remove every other package, and a package from before keyed
	// directories, past the ones a running game holds.
	entries, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if name := entry.Name(); name != keep && name != lockName {
			_ = os.RemoveAll(filepath.Join(dir, name))
		}
	}
	return nil
}

// Hold marks a session live in the installation at gameDir until the returned
// file closes or the process exits, however it exits. Hold before mounting
// and close after unmounting. Several sessions may hold at once.
func Hold(gameDir string) (*os.File, error) {
	dir := Path(gameDir)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	return filelock.Lock(filepath.Join(dir, lockName), false, true)
}

// Mount adds the session's block for the package named key to the
// installation's gameinfo.gi, replacing a block a session left that did not
// end cleanly or that names an older package.
func Mount(gameDir, key string) error {
	return edit(gameDir, func(source []byte) ([]byte, error) {
		return gameinfo.Mount(source, Name, Dir+"/"+key)
	})
}

// Unmount removes the session's block from the installation's gameinfo.gi,
// as it reads now, keeping every change another program made since.
func Unmount(gameDir string) error {
	return edit(gameDir, func(source []byte) ([]byte, error) {
		return gameinfo.Unmount(source, Name)
	})
}

// edit rewrites the installation's gameinfo.gi with change, as it reads now,
// and leaves the file untouched when change returns it unchanged. A running
// game holds the file for a moment when it reloads its search paths, as it
// does when the session's server stops under it, so edit tries again while
// the file is held.
func edit(gameDir string, change func(source []byte) ([]byte, error)) error {
	info := infoPath(gameDir)
	return fileretry.Do(func() error {
		source, err := os.ReadFile(info)
		if err != nil {
			return err
		}
		changed, err := change(source)
		if err != nil || bytes.Equal(changed, source) {
			return err
		}
		return os.WriteFile(info, changed, 0o644)
	})
}

// ClearStale unmounts a block that no live session holds, and returns
// ErrRunning when one does. A block without a lock file came from a version
// before the lock and counts as stale.
func ClearStale(gameDir string) error {
	// Leave an installation without the block untouched.
	source, err := os.ReadFile(infoPath(gameDir))
	if err != nil {
		return err
	}
	unmounted, err := gameinfo.Unmount(source, Name)
	if err != nil || bytes.Equal(unmounted, source) {
		return err
	}

	// Unmount only while no session holds the lock.
	dir := Path(gameDir)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	held, err := filelock.Lock(filepath.Join(dir, lockName), true, false)
	if err == filelock.ErrLocked {
		return ErrRunning
	}
	if err != nil {
		return err
	}
	defer held.Close()
	return Unmount(gameDir)
}

// infoPath returns the installation's gameinfo.gi.
func infoPath(gameDir string) string {
	return filepath.Join(gameDir, "game", "citadel", "gameinfo.gi")
}
