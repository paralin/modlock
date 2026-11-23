package controllers

import (
	"errors"
	"math"
	"os"
	"runtime"
	"time"
	"unsafe"
)

// VirtualEnv names the environment variable that, set to 1, makes every
// Reader in the process stream two virtual devices beside the real ones, so
// a controller mod can be built and measured on a PC with no device:
//
//   - "Modlock virtual gamepad", a gamepad whose left stick's x traces the
//     wall clock, (t mod 2 s) / 1 s - 1, so a mod decodes when the value was
//     read as (x + 1) seconds past an even second and compares it with its own
//     clock. Its left stick's y and its right stick circle once every 4 s, its
//     triggers ramp up over each second, and its south button is tapped for
//     one sample at each quarter second, a press and a release shorter than a
//     server frame.
//   - "Modlock virtual radio", a joystick with eight axes, four buttons and a
//     hat, as a radio transmitter in USB joystick mode looks. Axis k is
//     sin(2π t / 4 s + kπ / 4), button k is held during second k of each 4 s,
//     and the hat points up, right, down and left in turn, a second each.
const VirtualEnv = "MODLOCK_VIRTUAL_CONTROLLERS"

// virtualPeriod is the clock axis's period.
const virtualPeriod = 2 * time.Second

// virtualDevices are the devices VirtualEnv asks for, open so the reader can
// move their inputs.
type virtualDevices struct {
	lib *sdlLib
	// ids are the attached devices; pad and radio are handles to them.
	ids        []uint32
	pad, radio uintptr
	// tap is the quarter second whose tap began, and tapped whether the
	// button is still down from it.
	tap    int64
	tapped bool
}

// wantVirtual reports whether VirtualEnv asks for virtual devices.
func wantVirtual() bool {
	return os.Getenv(VirtualEnv) == "1"
}

// openVirtual attaches the virtual gamepad and radio. Callers run on the
// thread that started SDL's joystick layer.
func openVirtual(lib *sdlLib) (*virtualDevices, error) {
	// Attach both, or neither.
	v := &virtualDevices{lib: lib}
	var err error
	if v.pad, err = v.attach("Modlock virtual gamepad", true, 6, 15, 0); err == nil {
		v.radio, err = v.attach("Modlock virtual radio", false, 8, 4, 1)
	}
	if err != nil {
		v.detach()
		return nil, err
	}
	return v, nil
}

// attach attaches one device and opens a handle to move its inputs.
func (v *virtualDevices) attach(name string, gamepad bool, axes, buttons, hats uint16) (uintptr, error) {
	// Describe the device; SDL copies the name.
	cname := append([]byte(name), 0)
	desc := sdlVirtualJoystickDesc{
		version:  uint32(unsafe.Sizeof(sdlVirtualJoystickDesc{})),
		naxes:    axes,
		nbuttons: buttons,
		nhats:    hats,
		name:     &cname[0],
	}
	if gamepad {
		desc.kind = sdlJoystickTypeGamepad
	}

	// Attach it and open the handle the reader moves it through.
	id := v.lib.AttachVirtualJoystick(&desc)
	runtime.KeepAlive(cname)
	if id == 0 {
		return 0, errors.New("controllers: attach " + name + ": " + v.lib.GetError())
	}
	v.ids = append(v.ids, id)
	js := v.lib.OpenJoystick(id)
	if js == 0 {
		return 0, errors.New("controllers: open " + name + ": " + v.lib.GetError())
	}
	return js, nil
}

// detach closes and detaches the devices.
func (v *virtualDevices) detach() {
	for _, js := range []uintptr{v.pad, v.radio} {
		if js != 0 {
			v.lib.CloseJoystick(js)
		}
	}
	for _, id := range v.ids {
		v.lib.DetachVirtualJoystick(id)
	}
}

// set moves every input to its value at now, which SDL reports at its next
// update.
func (v *virtualDevices) set(now time.Time) {
	// Trace the clock on the pad's left x, and circle the other sticks.
	ms := now.UnixMilli()
	phase := float64(now.UnixNano()%int64(virtualPeriod)) / float64(time.Second)
	turn := 2 * math.Pi * float64(now.UnixNano()%int64(4*time.Second)) / float64(4*time.Second)
	lib := v.lib
	lib.SetJoystickVirtualAxis(v.pad, 0, axisValue(phase-1))
	lib.SetJoystickVirtualAxis(v.pad, 1, axisValue(math.Sin(turn)))
	lib.SetJoystickVirtualAxis(v.pad, 2, axisValue(math.Cos(turn)))
	lib.SetJoystickVirtualAxis(v.pad, 3, axisValue(-math.Sin(turn)))

	// Ramp the triggers, and tap the south button once a quarter second.
	ramp := float64(ms%1000) / 1000
	lib.SetJoystickVirtualAxis(v.pad, 4, axisValue(ramp))
	lib.SetJoystickVirtualAxis(v.pad, 5, axisValue(ramp))
	quarter := ms / 250
	switch {
	case quarter != v.tap:
		v.tap, v.tapped = quarter, true
		lib.SetJoystickVirtualButton(v.pad, 0, true)
	case v.tapped:
		v.tapped = false
		lib.SetJoystickVirtualButton(v.pad, 0, false)
	}

	// Wave the radio's axes, hold one button a second, and turn its hat.
	for k := range 8 {
		lib.SetJoystickVirtualAxis(v.radio, int32(k), axisValue(math.Sin(turn+float64(k)*math.Pi/4)))
	}
	second := (ms / 1000) % 4
	for k := range int64(4) {
		lib.SetJoystickVirtualButton(v.radio, int32(k), k == second)
	}
	lib.SetJoystickVirtualHat(v.radio, 0, uint8(1)<<second)
}

// axisValue scales x from -1 to 1 to an SDL axis value.
func axisValue(x float64) int16 {
	return int16(math.Round(max(-1, min(1, x)) * 32767))
}
