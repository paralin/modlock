//go:build windows && amd64

package controllers

import (
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"os"
	"path/filepath"
	"syscall"
	"unsafe"

	"github.com/paralin/modlock/internal/fetch"
)

// built holds the SDL3.dll a release build embeds; in any other build it
// holds only the directory's .gitignore.
//
//go:embed all:built
var built embed.FS

// loadSDL loads SDL3.dll, from beside the executable, then the one this build
// embeds, then the system's search path, and resolves the reader's functions.
func loadSDL() (*sdlLib, error) {
	// List where SDL3.dll could be.
	var names []string
	if exe, err := os.Executable(); err == nil {
		names = append(names, filepath.Join(filepath.Dir(exe), "SDL3.dll"))
	}
	if embedded, err := installEmbedded(); err == nil && embedded != "" {
		names = append(names, embedded)
	}
	names = append(names, "SDL3.dll")

	// Load the first that opens.
	for _, name := range names {
		handle, err := syscall.LoadLibrary(name)
		if err != nil {
			continue
		}
		lib := &sdlLib{}
		lib.register(uintptr(handle))

		// Windows x64 returns a 16-byte struct through a pointer the caller
		// passes first, which purego does not do there, so call it directly.
		fn, err := syscall.GetProcAddress(handle, "SDL_GetJoystickGUID")
		if err != nil {
			return nil, err
		}
		lib.guid = func(js uintptr) sdlGUID {
			var guid sdlGUID
			syscall.SyscallN(fn, uintptr(unsafe.Pointer(&guid)), js)
			return guid
		}
		return lib, nil
	}
	return nil, ErrUnavailable
}

// installEmbedded copies the embedded SDL3.dll into the user cache, keyed by
// its contents, the first time, and returns its path, or empty when this
// build embeds none.
func installEmbedded() (string, error) {
	// Name the cache directory by the library's checksum.
	data, err := built.ReadFile("built/SDL3.dll")
	if err != nil {
		return "", nil
	}
	sum := sha256.Sum256(data)
	dir, err := fetch.Cache("sdl3", hex.EncodeToString(sum[:])[:16])
	if err != nil {
		return "", err
	}

	// Write it into place whole.
	err = fetch.Install(dir, func(staging string) error {
		if err := os.MkdirAll(staging, 0o755); err != nil {
			return err
		}
		return os.WriteFile(filepath.Join(staging, "SDL3.dll"), data, 0o644)
	})
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "SDL3.dll"), nil
}
