// Command extension-go serves the echo service, which answers each call with
// the host's shout service's answer to the same payload; the hold service,
// whose methods take and release the hold on its running build; and the
// crash service, which panics and so stops the mod.
package main

import "github.com/paralin/modlock/mod"

// init registers the service before the host starts the mod.
func init() {
	mod.Serve("echo", func(method string, payload []byte) ([]byte, error) {
		return mod.CallService("shout", method, payload)
	})
	mod.Serve("hold", func(method string, _ []byte) ([]byte, error) {
		return nil, mod.HoldReload(method == "take")
	})
	mod.Serve("crash", func(string, []byte) ([]byte, error) {
		panic("crash")
	})
}

// main is required by Go and never runs; the host calls the service.
func main() {}
