//go:build windows && amd64

package controllers

import (
	"context"
	"errors"
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
// search path. A failed download is returned with ErrUnavailable when nothing
// else loads.
func loadSDL() (*sdlLib, error) {
	failures := []error{ErrUnavailable}

	// Prefer the library the application ships beside its executable.
	if exe, err := os.Executable(); err == nil {
		if lib, err := openSDL(filepath.Join(filepath.Dir(exe), "SDL3.dll")); err == nil {
			return lib, nil
		} else {
			failures = append(failures, err)
		}
	}

	// Then the pinned release, downloaded once.
	pinned, installErr := installPinned()
	if installErr == nil {
		if lib, err := openSDL(pinned); err == nil {
			return lib, nil
		} else {
			failures = append(failures, err)
		}
	}

	// Then whatever the search path finds.
	if lib, err := openSDL("SDL3.dll"); err == nil {
		return lib, nil
	} else {
		failures = append(failures, err)
	}
	if installErr != nil {
		failures = append(failures, installErr)
	}
	return nil, errors.Join(failures...)
}

// openSDL loads the SDL3 library at name and resolves the reader's functions.
func openSDL(name string) (*sdlLib, error) {
	// Open the library without publishing any of its functions yet.
	handle, err := syscall.LoadLibrary(name)
	if err != nil {
		return nil, err
	}

	// Release an incomplete library before trying the next candidate.
	lib := &sdlLib{}
	if err := lib.register(func(name string) (uintptr, error) { return syscall.GetProcAddress(handle, name) }); err != nil {
		syscall.FreeLibrary(handle)
		return nil, err
	}

	// Windows x64 returns a 16-byte struct through a pointer the caller
	// passes first, which purego does not do there, so call it directly.
	fn, err := syscall.GetProcAddress(handle, "SDL_GetJoystickGUID")
	if err != nil {
		syscall.FreeLibrary(handle)
		return nil, errors.New("controllers: resolve SDL_GetJoystickGUID: " + err.Error())
	}
	lib.guid = func(js uintptr) sdlGUID {
		var guid sdlGUID
		syscall.SyscallN(fn, uintptr(unsafe.Pointer(&guid)), js)
		return guid
	}
	return lib, nil
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
