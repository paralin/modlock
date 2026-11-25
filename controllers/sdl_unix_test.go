//go:build darwin || linux

package controllers

import (
	"runtime"
	"strings"
	"testing"
)

// TestIncompleteSDLLibrary loads a real library without SDL symbols and keeps
// its missing-symbol diagnostic instead of panicking in the process loader.
func TestIncompleteSDLLibrary(t *testing.T) {
	name := "libc.so.6"
	if runtime.GOOS == "darwin" {
		name = "/usr/lib/libSystem.B.dylib"
	}
	lib, err := openSDL(name)
	if lib != nil || err == nil || !strings.Contains(err.Error(), "SDL_SetHint") {
		t.Fatalf("incomplete SDL library = %v, %v; want missing SDL_SetHint", lib, err)
	}
}
