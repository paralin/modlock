package controllers

import (
	"context"
	"errors"
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
	defer s.stop()
	for {
		select {
		case <-ctx.Done():
			return nil
		case err := <-s.done:
			s.cancel = nil
			if err != nil {
				return err
			}
		case epoch, ok := <-wants:
			if !ok {
				return nil
			}
			if err := s.want(epoch); err != nil {
				return err
			}
		}
	}
}

// server runs one Reader for Serve while a want holds.
type server struct {
	ctx  context.Context
	log  *slog.Logger
	send func(*pb.ControllerFrame) error

	// reader is open once a want arrived and SDL3 loaded; missing is true
	// once it failed to.
	reader  *Reader
	missing bool

	// cancel stops the running Run, which reports on done; nil while none
	// runs.
	cancel context.CancelFunc
	done   chan error
}

// want starts the reader for a nonzero epoch, or stops it for zero, and
// returns a send error from a Run it stopped.
func (s *server) want(epoch uint32) error {
	// Stop reading while nothing is wanted.
	if epoch == 0 {
		return s.stop()
	}

	// Open SDL3 on the first want, or log once that it is missing.
	if s.reader == nil && !s.missing {
		reader, err := Open(s.log)
		if err != nil {
			s.missing = true
			if errors.Is(err, ErrUnavailable) {
				s.log.Info("controllers unavailable: SDL3 is not installed")
			} else {
				s.log.Warn("controllers unavailable", "err", err)
			}
			return nil
		}
		s.reader = reader
	}
	if s.reader == nil {
		return nil
	}

	// Answer the epoch, starting the reader if it is not running.
	s.reader.Want(epoch)
	if s.cancel == nil {
		s.log.Info("controllers: reading devices", "epoch", epoch)
		run, cancel := context.WithCancel(s.ctx)
		s.cancel = cancel
		go func() { s.done <- s.reader.Run(run, s.send) }()
	}
	return nil
}

// stop ends a running Run, closing its devices, and returns its error.
func (s *server) stop() error {
	// Skip a reader that is not running.
	if s.cancel == nil {
		return nil
	}

	// Cancel it and wait for its devices to close.
	s.cancel()
	s.cancel = nil
	err := <-s.done
	s.log.Info("controllers: closed devices")
	return err
}
