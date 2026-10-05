// Command extension-go serves the echo service, which answers each call with
// the host's shout service's answer to the same payload.
package main

import "github.com/paralin/modlock/mod"

// init registers the service before the host starts the mod.
func init() {
	mod.Serve("echo", func(method string, payload []byte) ([]byte, error) {
		return mod.CallService("shout", method, payload)
	})
}

// main is required by Go and never runs; the host calls the service.
func main() {}
