package controllers

import "sync"

// sdlCache keeps the first successfully loaded library for the process.
// Failed loads leave the cache empty so later demand can retry.
type sdlCache struct {
	// Mutex serializes the automatic loader and application-supplied path.
	sync.Mutex
	// lib is the successful library, or nil before a successful load.
	lib *sdlLib
}

// load returns the cached library or attempts the normal platform loader.
func (c *sdlCache) load() (*sdlLib, error) {
	c.Lock()
	defer c.Unlock()
	if c.lib == nil {
		lib, err := loadSDL()
		if err != nil {
			return nil, err
		}
		c.lib = lib
	}
	return c.lib, nil
}

// loadPath sets the application library only before a successful load.
func (c *sdlCache) loadPath(path string) error {
	c.Lock()
	defer c.Unlock()
	if c.lib == nil {
		lib, err := openSDL(path)
		if err != nil {
			return err
		}
		c.lib = lib
	}
	return nil
}
