package mod

import (
	"errors"
	"time"
)

// Service answers calls to one service the mod serves: the method's name and
// payload in, the answer out.
type Service func(method string, payload []byte) ([]byte, error)

// ServiceError ends a started service call without an answer: Failure says
// why and Message describes it.
type ServiceError struct {
	// Failure classifies the reason the call ended.
	Failure ServiceFailure
	// Message describes that reason.
	Message string
}

// Error returns the failure's description.
func (e *ServiceError) Error() string {
	return e.Message
}

// CallServiceLater calls method of a service the host provides and returns
// at once; done runs in a later frame with the answer, or with a
// *ServiceError when the service refused, the call waited past timeout or
// nobody provides the service. A zero timeout waits the host's longest, 30
// seconds. done runs exactly once while the mod runs; calls still waiting
// when the mod stops end with it.
func CallServiceLater(service, method string, payload []byte, timeout time.Duration, done func(reply []byte, err error)) {
	calls.start(service, method, payload, timeout, done)
}

// errorOf returns message as an error, or nil when it is empty.
func errorOf(message string) error {
	if message == "" {
		return nil
	}
	return errors.New(message)
}
