package controllers

import (
	"context"
	"log/slog"

	pb "github.com/paralin/modlock/src/wasm/controllers"
)

// server runs one Reader for Serve while a want holds.
type server struct {
	ctx  context.Context
	log  *slog.Logger
	send func(*pb.ControllerFrame) error

	// reader is open once a want arrived and SDL3 loaded; missing is true
	// once a load failed, so the failure logs once while later wants retry.
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

	// Open SDL3 on a want, logging the first failure; each later want tries
	// again.
	if s.reader == nil {
		reader, err := Open(s.log)
		if err != nil {
			if !s.missing {
				s.missing = true
				s.log.Info("controllers unavailable", "err", err)
			}
			return nil
		}
		s.reader = reader
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

// serve follows demand and consumes the completion of the reader it starts.
func (s *server) serve(wants <-chan uint32) error {
	defer s.stop()
	for {
		select {
		case <-s.ctx.Done():
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
