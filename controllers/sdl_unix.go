//go:build darwin || linux

package controllers

import (
	"errors"
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
	// Keep each candidate's diagnostic while trying the remaining libraries.
	failures := []error{ErrUnavailable}
	for _, name := range sdlNames() {
		lib, err := openSDL(name)
		if err == nil {
			return lib, nil
		}
		failures = append(failures, err)
	}
	return nil, errors.Join(failures...)
}

// openSDL loads the SDL3 library at name and resolves the reader's functions.
func openSDL(name string) (*sdlLib, error) {
	// Open the library without publishing any of its functions yet.
	handle, err := purego.Dlopen(name, purego.RTLD_NOW|purego.RTLD_LOCAL)
	if err != nil {
		return nil, err
	}

	// Release an incomplete library before trying the next candidate.
	lib := &sdlLib{}
	if err := lib.register(func(name string) (uintptr, error) { return purego.Dlsym(handle, name) }); err != nil {
		purego.Dlclose(handle)
		return nil, err
	}

	// Resolve the struct-returning function with the Unix calling convention.
	var guid func(js uintptr) sdlGUID
	addr, err := purego.Dlsym(handle, "SDL_GetJoystickGUID")
	if err != nil {
		purego.Dlclose(handle)
		return nil, errors.New("controllers: resolve SDL_GetJoystickGUID: " + err.Error())
	}
	purego.RegisterFunc(&guid, addr)
	lib.guid = guid
	return lib, nil
}
