package session

import "github.com/pkg/errors"

// ErrNoGame marks the errors that mean this computer cannot run a Deadlock
// server: the system is not Windows or Linux, or Steam or the game is missing.
var ErrNoGame = errors.New("this computer cannot run Deadlock")

// noGameError is an error that means the game cannot run here.
type noGameError struct {
	// error is the cause, whose message the error keeps.
	error
}

// noGame marks err with ErrNoGame.
func noGame(err error) error {
	return &noGameError{err}
}

// Is matches ErrNoGame.
func (e *noGameError) Is(target error) bool {
	return target == ErrNoGame
}

// Unwrap returns the cause.
func (e *noGameError) Unwrap() error {
	return e.error
}
