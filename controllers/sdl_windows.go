//go:build windows && amd64

package controllers

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"syscall"
	"time"
	"unsafe"

	"github.com/paralin/modlock/internal/fetch"
)

// installTimeout bounds the download of SDL3.dll on a PC that lacks it.
const installTimeout = time.Minute

// loadSDL loads SDL3.dll, from beside the executable, then the pinned release
// in the user cache, downloading it there when missing, then the system's
// search path, and resolves the reader's functions. A failed download is
// returned with ErrUnavailable when nothing else loads.
func loadSDL() (*sdlLib, error) {
	// List where SDL3.dll could be.
	var names []string
	if exe, err := os.Executable(); err == nil {
		names = append(names, filepath.Join(filepath.Dir(exe), "SDL3.dll"))
	}
	pinned, installErr := installPinned()
	if installErr == nil {
		names = append(names, pinned)
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
	if installErr != nil {
		return nil, fmt.Errorf("%w: %v", ErrUnavailable, installErr)
	}
	return nil, ErrUnavailable
}

// installPinned returns the path of the pinned SDL3.dll in the user cache,
// downloading it there the first time.
func installPinned() (string, error) {
	dir, err := fetch.Cache("sdl3", SDLVersion)
	if err != nil {
		return "", err
	}
	ctx, cancel := context.WithTimeout(context.Background(), installTimeout)
	defer cancel()
	if err := InstallSDL(ctx, dir); err != nil {
		return "", err
	}
	return filepath.Join(dir, "SDL3.dll"), nil
}
