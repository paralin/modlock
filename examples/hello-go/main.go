// Command hello-go is the smallest Modlock mod in Go: it answers /hello in
// chat and reports the first server frame.
package main

import "github.com/paralin/modlock/mod"

// init registers the mod's handlers before the server starts it.
func init() {
	// Greet each player who types /hello.
	mod.Command("hello", func(p mod.Player, args string) {
		_ = p.Chat("Hello from Go!")
	})

	// Report the first server frame once.
	seen := false
	mod.OnFrame(func(f *mod.FrameEvent) {
		if seen {
			return
		}
		seen = true
		mod.Log("first frame at tick ", f.GetTick())
	})
}

// main is required by Go and never runs; the server calls the handlers.
func main() {}
