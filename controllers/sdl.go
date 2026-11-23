package controllers

import (
	"encoding/binary"
	"encoding/hex"
	"errors"
	"unsafe"

	"github.com/ebitengine/purego"
)

// ErrUnavailable is returned by Open when no SDL3 library can be loaded, so a
// client without it runs on and streams no controllers.
var ErrUnavailable = errors.New("controllers: SDL3 is not installed")

// SDL subsystem flags, from SDL_init.h.
const (
	sdlInitJoystick = 0x200
	sdlInitGamepad  = 0x2000
)

// SDL event types, from SDL_events.h.
const (
	sdlEventJoystickButtonDown = 0x603
	sdlEventJoystickButtonUp   = 0x604
	sdlEventJoystickAdded      = 0x605
	sdlEventJoystickRemoved    = 0x606
	sdlEventGamepadButtonDown  = 0x651
	sdlEventGamepadButtonUp    = 0x652
	sdlEventUser               = 0x8000
)

// sdlGamepadAxisCount is the number of SDL_GamepadAxis values: the two sticks'
// x and y, then the left and right triggers.
const sdlGamepadAxisCount = 6

// sdlGamepadButtonCount is the number of SDL_GamepadButton values.
const sdlGamepadButtonCount = 26

// sdlEvent is SDL_Event, a 128-byte union whose first field is its type. A
// device or button event keeps the joystick id at byte 16, and a button
// event the button at byte 20.
type sdlEvent [16]uint64

// kind returns the event's type.
func (e *sdlEvent) kind() uint32 {
	return uint32(e[0])
}

// which returns the joystick id of a device or button event.
func (e *sdlEvent) which() uint32 {
	return uint32(e[2])
}

// button returns the button of a button event.
func (e *sdlEvent) button() uint8 {
	return uint8(e[2] >> 32)
}

// sdlGUID is SDL_GUID, sixteen bytes returned by value. Two words keep it in
// the two return registers purego reads on amd64 and arm64.
type sdlGUID struct {
	lo, hi uint64
}

// String formats the GUID as SDL_GUIDToString does, its bytes in hexadecimal.
func (g sdlGUID) String() string {
	var b [16]byte
	binary.LittleEndian.PutUint64(b[:8], g.lo)
	binary.LittleEndian.PutUint64(b[8:], g.hi)
	return hex.EncodeToString(b[:])
}

// sdlLib holds the SDL3 functions the reader calls, resolved from a loaded
// library. Every pointer is an SDL handle owned by SDL.
type sdlLib struct {
	SetHint          func(name, value string) bool
	InitSubSystem    func(flags uint32) bool
	QuitSubSystem    func(flags uint32)
	GetError         func() string
	Free             func(mem unsafe.Pointer)
	WaitEventTimeout func(event *sdlEvent, ms int32) bool
	PollEvent        func(event *sdlEvent) bool
	PushEvent        func(event *sdlEvent) bool

	GetJoysticks          func(count *int32) unsafe.Pointer
	IsGamepad             func(id uint32) bool
	OpenJoystick          func(id uint32) uintptr
	CloseJoystick         func(js uintptr)
	OpenGamepad           func(id uint32) uintptr
	CloseGamepad          func(pad uintptr)
	GetGamepadJoystick    func(pad uintptr) uintptr
	GetJoystickName       func(js uintptr) string
	GetJoystickVendor     func(js uintptr) uint16
	GetJoystickProduct    func(js uintptr) uint16
	GetNumJoystickAxes    func(js uintptr) int32
	GetNumJoystickButtons func(js uintptr) int32
	GetNumJoystickHats    func(js uintptr) int32
	GetJoystickAxis       func(js uintptr, axis int32) int16
	GetJoystickButton     func(js uintptr, button int32) bool
	GetJoystickHat        func(js uintptr, hat int32) uint8
	GetGamepadAxis        func(pad uintptr, axis int32) int16
	GetGamepadButton      func(pad uintptr, button int32) bool

	// guid returns SDL_GetJoystickGUID(js), whose struct return each
	// platform's file calls in its own way.
	guid func(js uintptr) sdlGUID

	// Virtual devices, for tests and MODLOCK_VIRTUAL_CONTROLLERS.
	AttachVirtualJoystick    func(desc *sdlVirtualJoystickDesc) uint32
	DetachVirtualJoystick    func(id uint32) bool
	SetJoystickVirtualAxis   func(js uintptr, axis int32, value int16) bool
	SetJoystickVirtualButton func(js uintptr, button int32, down bool) bool
	SetJoystickVirtualHat    func(js uintptr, hat int32, value uint8) bool
	UpdateJoysticks          func()
}

// register resolves every function in lib from the library handle, and
// panics, as purego does, when one is missing.
func (lib *sdlLib) register(handle uintptr) {
	for _, fn := range []struct {
		ptr  any
		name string
	}{
		{&lib.SetHint, "SDL_SetHint"},
		{&lib.InitSubSystem, "SDL_InitSubSystem"},
		{&lib.QuitSubSystem, "SDL_QuitSubSystem"},
		{&lib.GetError, "SDL_GetError"},
		{&lib.Free, "SDL_free"},
		{&lib.WaitEventTimeout, "SDL_WaitEventTimeout"},
		{&lib.PollEvent, "SDL_PollEvent"},
		{&lib.PushEvent, "SDL_PushEvent"},
		{&lib.GetJoysticks, "SDL_GetJoysticks"},
		{&lib.IsGamepad, "SDL_IsGamepad"},
		{&lib.OpenJoystick, "SDL_OpenJoystick"},
		{&lib.CloseJoystick, "SDL_CloseJoystick"},
		{&lib.OpenGamepad, "SDL_OpenGamepad"},
		{&lib.CloseGamepad, "SDL_CloseGamepad"},
		{&lib.GetGamepadJoystick, "SDL_GetGamepadJoystick"},
		{&lib.GetJoystickName, "SDL_GetJoystickName"},
		{&lib.GetJoystickVendor, "SDL_GetJoystickVendor"},
		{&lib.GetJoystickProduct, "SDL_GetJoystickProduct"},
		{&lib.GetNumJoystickAxes, "SDL_GetNumJoystickAxes"},
		{&lib.GetNumJoystickButtons, "SDL_GetNumJoystickButtons"},
		{&lib.GetNumJoystickHats, "SDL_GetNumJoystickHats"},
		{&lib.GetJoystickAxis, "SDL_GetJoystickAxis"},
		{&lib.GetJoystickButton, "SDL_GetJoystickButton"},
		{&lib.GetJoystickHat, "SDL_GetJoystickHat"},
		{&lib.GetGamepadAxis, "SDL_GetGamepadAxis"},
		{&lib.GetGamepadButton, "SDL_GetGamepadButton"},
		{&lib.AttachVirtualJoystick, "SDL_AttachVirtualJoystick"},
		{&lib.DetachVirtualJoystick, "SDL_DetachVirtualJoystick"},
		{&lib.SetJoystickVirtualAxis, "SDL_SetJoystickVirtualAxis"},
		{&lib.SetJoystickVirtualButton, "SDL_SetJoystickVirtualButton"},
		{&lib.SetJoystickVirtualHat, "SDL_SetJoystickVirtualHat"},
		{&lib.UpdateJoysticks, "SDL_UpdateJoysticks"},
	} {
		purego.RegisterLibFunc(fn.ptr, handle, fn.name)
	}
}

// joysticks returns the ids of the attached joysticks.
func (lib *sdlLib) joysticks() []uint32 {
	// Ask SDL for the list, which the caller frees.
	var count int32
	ids := lib.GetJoysticks(&count)
	if ids == nil {
		return nil
	}
	defer lib.Free(ids)

	// Copy it out of SDL's memory.
	return append([]uint32(nil), unsafe.Slice((*uint32)(ids), count)...)
}

// wake pushes a user event, which ends a wait on the thread reading events.
// It does nothing while the event queue is not running.
func (lib *sdlLib) wake() {
	ev := sdlEvent{sdlEventUser}
	lib.PushEvent(&ev)
}

// sdlVirtualJoystickDesc is SDL_VirtualJoystickDesc on a 64-bit platform: its
// version is its size, 136 bytes, and every callback is left unset.
type sdlVirtualJoystickDesc struct {
	version    uint32
	kind       uint16
	_          uint16
	vendor     uint16
	product    uint16
	naxes      uint16
	nbuttons   uint16
	nballs     uint16
	nhats      uint16
	ntouchpads uint16
	nsensors   uint16
	_          [2]uint16
	buttonMask uint32
	axisMask   uint32
	_          uint32
	name       *byte
	_          [11]uintptr
}

// sdlJoystickTypeGamepad is SDL_JOYSTICK_TYPE_GAMEPAD.
const sdlJoystickTypeGamepad = 1
