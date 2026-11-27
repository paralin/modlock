package mod

import "time"

// serviceCalls owns pending host-service completions, separate from inbound services.
type serviceCalls struct {
	// waiting maps a started call number to its completion callback.
	waiting map[uint64]func(reply []byte, err error)
}

// calls holds the default module's pending service calls.
var calls serviceCalls

// start registers a successful start or defers a refused start until the next frame.
func (c *serviceCalls) start(service, method string, payload []byte, timeout time.Duration, done func([]byte, error)) {
	// A call the host refuses to start ends in the next frame as well, so
	// done never runs before this returns.
	call, err := StartServiceCall(service, method, payload, uint32(timeout.Milliseconds()))
	if err != nil {
		After(0, func() { done(nil, &ServiceError{Failure: ServiceFailureUnavailable, Message: err.Error()}) })
		return
	}

	// Keep the completion only after the host accepted the call.
	if c.waiting == nil {
		c.waiting = map[uint64]func([]byte, error){}
	}
	c.waiting[call] = done
}

// ServiceReplied ends one started service call.
func (h *serviceCalls) ServiceReplied(event *ServiceRepliedEvent) error {
	// Claim the completion before invoking code that can reenter delivery.
	done, ok := h.waiting[event.GetCall()]
	if !ok {
		return nil
	}
	delete(h.waiting, event.GetCall())
	if event.GetFailure() != ServiceFailureNone {
		done(nil, &ServiceError{Failure: event.GetFailure(), Message: event.GetError()})
		return nil
	}

	// Deliver the successful reply with its original payload.
	done(event.GetPayload(), nil)
	return nil
}
