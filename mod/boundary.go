//go:build wasip1

package mod

import (
	"runtime"
	"unsafe"

	"github.com/paralin/modlock/proto/modlock/wasm"
)

// hostCall hands the host one encoded Call and returns the length of the
// encoded Reply, which hostRead then copies out.
//
//go:wasmimport modlock host_call
func hostCall(data unsafe.Pointer, size uint32) uint32

// hostRead copies the host's pending message, a Call or a Reply, into data,
// which holds exactly the length the host announced.
//
//go:wasmimport modlock host_read
func hostRead(data unsafe.Pointer, size uint32)

// modlockEvent receives one encoded Call of size bytes, serves it, and returns
// the encoded Reply's address in the high 32 bits and its length in the low
// 32 bits.
//
//go:wasmexport modlock_event
func modlockEvent(size uint32) uint64 {
	// Copy the pending call out of the host and decode it.
	data := make([]byte, size)
	hostRead(unsafe.Pointer(unsafe.SliceData(data)), size)
	call := &wasm.Call{}
	if err := call.UnmarshalVT(data); err != nil {
		Log("modlock: cannot decode event: ", err)
		return 0
	}

	// Serve the call and keep its encoded reply for the host.
	reply, err := serveMod(registered, call).MarshalVT()
	if err != nil || len(reply) == 0 {
		return 0
	}
	registered.result = reply
	address := uint64(uintptr(unsafe.Pointer(unsafe.SliceData(reply))))
	return address<<32 | uint64(len(reply))
}

// hostExchange sends an encoded call to the host and returns its encoded
// reply.
func hostExchange(call []byte) ([]byte, error) {
	// Hand the call to the host, which answers with the reply length.
	size := hostCall(unsafe.Pointer(unsafe.SliceData(call)), uint32(len(call)))
	runtime.KeepAlive(call)

	// Copy the reply out of the host.
	reply := make([]byte, size)
	if size != 0 {
		hostRead(unsafe.Pointer(unsafe.SliceData(reply)), size)
	}
	return reply, nil
}
