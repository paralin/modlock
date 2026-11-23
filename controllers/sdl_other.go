//go:build !darwin && !linux && !(windows && amd64)

package controllers

// loadSDL reports SDL3 unavailable on a platform the reader does not load it
// on.
func loadSDL() (*sdlLib, error) {
	return nil, ErrUnavailable
}
