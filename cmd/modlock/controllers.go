package main

import (
	"github.com/aperturerobotics/cli"
	modcli "github.com/paralin/modlock/cli"
	"github.com/paralin/modlock/controllers"
	"github.com/paralin/modlock/session"
	pb "github.com/paralin/modlock/src/wasm/controllers"
)

// observeControllers feeds this PC's controllers to the session's own player
// when no daemon hosts the session; a hosted session's daemon feeds that
// player itself, so with feed set it does nothing. Call it before the session
// starts, so its hub sees each join, and pass the started server to the call
// it returns. The sandbox has no game connections, so it gets no feed.
func observeControllers(c *cli.Context, out *printer, feed *hostFeed) func(running server) {
	if feed != nil {
		return func(server) {}
	}

	// Bind the session's players from its events. The hub's send runs under
	// its lock, so frames queue for the goroutine that hands them to the
	// server.
	queued := make(chan *pb.ControllerFrames, 16)
	hub := controllers.NewHub(func(frames *pb.ControllerFrames) {
		select {
		case queued <- frames:
		case <-c.Context.Done():
		}
	})
	out.tap(func(event *modcli.Event) {
		if host := event.GetHost(); host != nil {
			hub.Follow(host)
		}
	})

	// Once the server runs, read the devices while its mods watch them.
	return func(running server) {
		game, ok := running.(*session.Session)
		if !ok {
			return
		}
		go func() {
			for {
				select {
				case frames := <-queued:
					if game.Controllers(c.Context, frames) != nil {
						return
					}
				case <-c.Context.Done():
					return
				}
			}
		}()
		go controllers.FeedLocal(c.Context, hub)
	}
}
