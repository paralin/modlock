// Command profiled spends known time in its handlers, for the profiler's
// tests: 2 ms each frame, 6 ms every 30th frame, and a repeat that spends
// 1 ms.
package main

import (
	"time"

	"github.com/paralin/modlock/mod"
)

// spin spends d of the mod's time.
func spin(d time.Duration) {
	for start := time.Now(); time.Since(start) < d; {
	}
}

// frames counts the frames the mod saw.
var frames int

// init registers the handlers.
func init() {
	mod.OnFrame(func(*mod.FrameEvent) {
		frames++
		if frames%30 == 0 {
			spin(6 * time.Millisecond)
			return
		}
		spin(2 * time.Millisecond)
	})
	mod.Every(250*time.Millisecond, func() { spin(time.Millisecond) })
}

// main never runs; the server calls the handlers.
func main() {}
