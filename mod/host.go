package mod

import (
	"errors"
	"fmt"

	"github.com/paralin/modlock/proto/modlock/wasm"
)

// message is a protobuf message with fast encoding.
type message interface {
	MarshalVT() ([]byte, error)
	UnmarshalVT([]byte) error
}

// Log writes one line to the server log under the mod's name. Arguments are
// formatted as by fmt.Sprint.
func Log(args ...any) {
	_ = logLine(fmt.Sprint(args...))
}

// invoke calls one Host method with request and decodes its answer into
// response, which is nil for a method that answers nothing.
func invoke(method string, request, response message) error {
	// Encode the call for the host boundary.
	data, err := request.MarshalVT()
	if err != nil {
		return err
	}
	data, err = (&wasm.Call{Method: method, Request: data}).MarshalVT()
	if err != nil {
		return err
	}

	// Exchange the call for the host's reply.
	data, err = hostExchange(data)
	if err != nil {
		return err
	}
	reply := &wasm.Reply{}
	if err := reply.UnmarshalVT(data); err != nil {
		return err
	}

	// Report the host's error or decode its response.
	if message := reply.GetError(); message != "" {
		return errors.New(message)
	}
	if response == nil {
		return nil
	}
	return response.UnmarshalVT(reply.GetResponse())
}

// answer returns the reply carrying a handler's response or error. A nil
// response answers nothing.
func answer(response message, err error) *wasm.Reply {
	if err != nil {
		return &wasm.Reply{Error: err.Error()}
	}
	if response == nil {
		return &wasm.Reply{}
	}
	data, err := response.MarshalVT()
	if err != nil {
		return &wasm.Reply{Error: err.Error()}
	}
	return &wasm.Reply{Response: data}
}
