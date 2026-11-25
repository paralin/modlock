//go:build darwin || linux

package controllers

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"

	"github.com/ebitengine/purego"
)

// sdlNames lists the SDL3 library names to try, beside the executable first
// and then where the system loader and package managers put it.
func sdlNames() []string {
	// Name the library as each system's loader does.
	name := "libSDL3.so.0"
	system := []string{"libSDL3.so.0", "libSDL3.so"}
	if runtime.GOOS == "darwin" {
		name = "libSDL3.dylib"
		system = []string{"libSDL3.dylib", "/opt/homebrew/lib/libSDL3.dylib", "/usr/local/lib/libSDL3.dylib"}
	}

	// Look beside the executable first.
	var names []string
	if exe, err := os.Executable(); err == nil {
		names = append(names, filepath.Join(filepath.Dir(exe), name))
	}
	return append(names, system...)
}

// loadSDL loads the first SDL3 library sdlNames finds.
func loadSDL() (*sdlLib, error) {
	for _, name := range sdlNames() {
		if lib, err := openSDL(name); err == nil {
			return lib, nil
		}
	}
	return nil, ErrUnavailable
}

// openSDL loads the SDL3 library at name and resolves the reader's functions.
func openSDL(name string) (*sdlLib, error) {
	handle, err := purego.Dlopen(name, purego.RTLD_NOW|purego.RTLD_LOCAL)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrUnavailable, err)
	}
	lib := &sdlLib{}
	lib.register(handle)
	var guid func(js uintptr) sdlGUID
	purego.RegisterLibFunc(&guid, handle, "SDL_GetJoystickGUID")
	lib.guid = guid
	return lib, nil
}
