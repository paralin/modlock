package controllers

import (
	"context"
	"log/slog"

	pb "github.com/paralin/modlock/src/wasm/controllers"
)

// Serve reads this PC's controllers while the latest want epoch is nonzero,
// passing each frame to send, until ctx ends, wants closes or send fails. A
// zero epoch closes the devices, so a player whose controllers no mod watches
// has none open. Where SDL3 is missing it logs once and reads nothing, which
// the host sees as a player with no stream.
func Serve(ctx context.Context, log *slog.Logger, wants <-chan uint32, send func(*pb.ControllerFrame) error) error {
	if log == nil {
		log = slog.Default()
	}
	s := &server{ctx: ctx, log: log, send: send, done: make(chan error, 1)}
	return s.serve(wants)
}
