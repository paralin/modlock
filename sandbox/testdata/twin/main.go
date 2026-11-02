// Command twin is a mod whose builds differ only where the linker says, for
// the sandbox's twin check. It applies three console variables at start in the
// order order names, and from frame 30 logs a line holding a random draw, the
// time, and dagger.
package main

import (
	"math/rand/v2"
	"strings"
	"time"

	"github.com/paralin/modlock/mod"
)

// order is the start commands, comma separated; a build may reorder them.
var order = "sv_cheats 1,citadel_koth_enabled 0,sv_hibernate_when_empty 0"

// dagger is the value the frame log carries; a build may change it.
var dagger = "30"

// init registers the handlers.
func init() {
	// Apply the console variables.
	mod.OnStart(func([]string) {
		for _, command := range strings.Split(order, ",") {
			_ = mod.ServerCommand(command)
		}
	})

	// Log the dagger, the entity's name, the draw and the time every
	// thirtieth frame.
	mod.OnFrame(func(f *mod.FrameEvent) {
		if f.Tick%30 == 0 {
			name, _ := mod.EntityName(7)
			mod.Log("dagger", dagger, name, rand.IntN(1000), time.Now().UnixMilli())
		}
	})
}

// main never runs; the server calls the handlers.
func main() {}
