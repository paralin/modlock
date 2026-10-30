// Command extension-go serves the echo service, which answers each call with
// the host's shout service's answer to the same payload; the hold service,
// whose methods take and release the hold on its running build; the reach
// service, whose "show" shows an entity only to the player in the slot its
// payload names and whose "command" runs a server command; the probe service,
// which makes the calls a host's call profile must count: an allowed and a
// refused console command, a refused file name, more distinct files than the
// profile keeps, and a method the host does not have; and the crash service,
// which panics and so stops the mod.
package main

import (
	"strconv"
	"unsafe"

	"github.com/paralin/modlock/mod"
	"github.com/paralin/modlock/src/wasm"
)

// hostCall and hostRead reach the host boundary directly, as the mod library
// does, so the probe can call a method the library has no function for.
//
//go:wasmimport modlock host_call
func hostCall(data unsafe.Pointer, size uint32) uint32

//go:wasmimport modlock host_read
func hostRead(data unsafe.Pointer, size uint32)

// init registers the service before the host starts the mod.
func init() {
	mod.Serve("echo", func(method string, payload []byte) ([]byte, error) {
		return mod.CallService("shout", method, payload)
	})
	mod.Serve("hold", func(method string, _ []byte) ([]byte, error) {
		return nil, mod.HoldReload(method == "take")
	})
	mod.Serve("reach", func(method string, payload []byte) ([]byte, error) {
		if method == "command" {
			return nil, mod.ServerCommand("echo")
		}
		slot, err := strconv.Atoi(string(payload))
		if err != nil {
			return nil, err
		}
		return nil, mod.ShowEntityOnlyTo(0, []int32{int32(slot)})
	})
	mod.Serve("probe", func(string, []byte) ([]byte, error) {
		call, err := (&wasm.Call{Method: "NoSuchMethod"}).MarshalVT()
		if err != nil {
			return nil, err
		}
		if size := hostCall(unsafe.Pointer(unsafe.SliceData(call)), uint32(len(call))); size != 0 {
			reply := make([]byte, size)
			hostRead(unsafe.Pointer(unsafe.SliceData(reply)), size)
		}
		_ = mod.ServerCommand("say hi; exec server.cfg")
		_, _ = mod.ReadFile("../outside")
		for i := range 300 {
			_, _ = mod.ReadFile("probe/" + strconv.Itoa(i))
		}
		return nil, nil
	})
	mod.Serve("crash", func(string, []byte) ([]byte, error) {
		panic("crash")
	})
}

// main is required by Go and never runs; the host calls the service.
func main() {}
