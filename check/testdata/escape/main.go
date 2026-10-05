// Command escape imports a function the host does not offer.
package main

// open is not a host function.
//
//go:wasmimport env open
func open() uint32

// modlockEvent keeps open reachable.
//
//go:wasmexport modlock_event
func modlockEvent(uint32) uint64 { return uint64(open()) }

// main never runs.
func main() {}
