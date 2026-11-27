package controllers

import (
	pb "github.com/paralin/modlock/src/wasm/controllers"
	"github.com/pkg/errors"
)

// device is one open SDL device: its joystick, its gamepad when SDL knows the
// layout, and what the host last received about it.
type device struct {
	info *pb.ControllerInfo
	js   uintptr
	pad  uintptr

	// announced is true once a frame carried info, and sent is the state
	// last sent, without its edges.
	announced bool
	sent      *pb.ControllerState

	// The presses and releases since the last frame, raw and gamepad.
	pressed, released       []uint32
	padPressed, padReleased []pb.GamepadButton
}

// openDevice opens joystick id, through the gamepad layer when SDL knows its
// layout. SDL opens a device without taking it from another program.
func openDevice(lib *sdlLib, id uint32) (*device, error) {
	// Open the gamepad and its joystick, or the joystick alone.
	d := &device{}
	if lib.IsGamepad(id) {
		if d.pad = lib.OpenGamepad(id); d.pad != 0 {
			d.js = lib.GetGamepadJoystick(d.pad)
		}
	}
	if d.js == 0 {
		d.close(lib)
		d.js = lib.OpenJoystick(id)
	}
	if d.js == 0 {
		return nil, errors.Errorf("open controller %d: %s", id, lib.GetError())
	}

	// Describe it as the host will list it.
	d.info = &pb.ControllerInfo{
		Id:      id,
		Name:    lib.GetJoystickName(d.js),
		Guid:    lib.guid(d.js).String(),
		Vendor:  uint32(lib.GetJoystickVendor(d.js)),
		Product: uint32(lib.GetJoystickProduct(d.js)),
		Axes:    uint32(max(lib.GetNumJoystickAxes(d.js), 0)),
		Buttons: uint32(max(lib.GetNumJoystickButtons(d.js), 0)),
		Hats:    uint32(max(lib.GetNumJoystickHats(d.js), 0)),
		Gamepad: d.pad != 0,
	}
	return d, nil
}

// close closes the device's handles and reports whether it was announced.
func (d *device) close(lib *sdlLib) bool {
	switch {
	case d.pad != 0:
		lib.CloseGamepad(d.pad)
	case d.js != 0:
		lib.CloseJoystick(d.js)
	}
	d.pad, d.js = 0, 0
	return d.announced
}

// edge records a button event: raw when pad is false, otherwise a gamepad
// button by SDL's number.
func (d *device) edge(button uint8, down, pad bool) {
	switch {
	case !pad && down:
		d.pressed = append(d.pressed, uint32(button))
	case !pad:
		d.released = append(d.released, uint32(button))
	default:
		named, ok := gamepadButton(int32(button))
		if !ok {
			return
		}
		if down {
			d.padPressed = append(d.padPressed, named)
			return
		}
		d.padReleased = append(d.padReleased, named)
	}
}

// hasEdges reports whether a press or release waits for the next frame.
func (d *device) hasEdges() bool {
	return len(d.pressed)+len(d.released)+len(d.padPressed)+len(d.padReleased) > 0
}

// clearEdges forgets the waiting presses and releases.
func (d *device) clearEdges() {
	d.pressed, d.released = d.pressed[:0], d.released[:0]
	d.padPressed, d.padReleased = d.padPressed[:0], d.padReleased[:0]
}

// read returns the device's inputs now, without edges. An unfocused read
// lists every input at rest.
func (d *device) read(lib *sdlLib, focused bool) *pb.ControllerState {
	// Read the raw layer.
	st := &pb.ControllerState{
		Id:      d.info.GetId(),
		Axes:    make([]float32, d.info.GetAxes()),
		Buttons: make([]bool, d.info.GetButtons()),
		Hats:    make([]uint32, d.info.GetHats()),
	}
	if d.pad != 0 {
		st.Gamepad = &pb.Gamepad{}
	}
	if !focused {
		return st
	}
	for i := range st.Axes {
		st.Axes[i] = unit(lib.GetJoystickAxis(d.js, int32(i)))
	}
	for i := range st.Buttons {
		st.Buttons[i] = lib.GetJoystickButton(d.js, int32(i))
	}
	for i := range st.Hats {
		st.Hats[i] = uint32(lib.GetJoystickHat(d.js, int32(i)))
	}

	// Read the gamepad layer.
	if d.pad == 0 {
		return st
	}
	var axes [sdlGamepadAxisCount]float32
	for i := range axes {
		axes[i] = unit(lib.GetGamepadAxis(d.pad, int32(i)))
	}
	st.Gamepad.LeftX, st.Gamepad.LeftY = axes[0], axes[1]
	st.Gamepad.RightX, st.Gamepad.RightY = axes[2], axes[3]
	st.Gamepad.LeftTrigger, st.Gamepad.RightTrigger = axes[4], axes[5]
	for b := range int32(sdlGamepadButtonCount) {
		if named, ok := gamepadButton(b); ok && lib.GetGamepadButton(d.pad, b) {
			st.Gamepad.Held = append(st.Gamepad.Held, named)
		}
	}
	return st
}

// withEdges returns st with the device's waiting presses and releases.
func (d *device) withEdges(st *pb.ControllerState) *pb.ControllerState {
	// Copy st, so the state kept as sent has no edges, and add them.
	out := st.CloneVT()
	out.Pressed = append(out.Pressed, d.pressed...)
	out.Released = append(out.Released, d.released...)
	if out.Gamepad != nil {
		out.Gamepad.Pressed = append(out.Gamepad.Pressed, d.padPressed...)
		out.Gamepad.Released = append(out.Gamepad.Released, d.padReleased...)
	}
	return out
}

// unit scales an SDL axis value to -1 to 1, a trigger's to 0 to 1.
func unit(v int16) float32 {
	return float32(max(v, -32767)) / 32767
}

// gamepadButton names SDL's gamepad button b, which is false for the
// paddles and the other buttons the layout leaves unnamed.
func gamepadButton(b int32) (pb.GamepadButton, bool) {
	switch {
	case b >= 0 && b <= 15:
		return pb.GamepadButton(b + 1), true
	case b == 20:
		return pb.GamepadButton_GAMEPAD_BUTTON_TOUCHPAD, true
	}
	return pb.GamepadButton_GAMEPAD_BUTTON_UNKNOWN, false
}

// appendFrame adds this device's announcement and changed state to frame.
// sent remains edge-free so a later frame cannot replay a delivered edge.
func (d *device) appendFrame(lib *sdlLib, frame *pb.ControllerFrame) {
	// A new epoch starts with current state instead of old queued edges.
	if frame.GetFull() {
		d.announced, d.sent = false, nil
		d.clearEdges()
	}

	// Announce the device before its first state.
	if !d.announced {
		frame.Added = append(frame.Added, d.info)
		d.announced = true
	}

	// Read the current inputs and skip an unchanged state without edges.
	st := d.read(lib, frame.GetFocused())
	if d.sent != nil && !d.hasEdges() && st.EqualVT(d.sent) {
		return
	}

	// Keep an edge-free baseline and consume queued edges exactly once.
	d.sent = st
	if frame.GetFocused() && d.hasEdges() {
		st = d.withEdges(st)
	}
	d.clearEdges()
	frame.Changed = append(frame.Changed, st)
}
