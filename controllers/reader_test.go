package controllers

import (
	"context"
	"errors"
	"log/slog"
	"maps"
	"runtime"
	"slices"
	"testing"
	"time"
	"unsafe"

	pb "github.com/paralin/modlock/src/wasm/controllers"
)

// openSDL returns the loaded library, skipping the test when SDL3 is not
// installed, with the joystick layers started on the test's locked thread.
func openSDL(t *testing.T) *sdlLib {
	t.Helper()
	r, err := Open(nil)
	if errors.Is(err, ErrUnavailable) {
		t.Skip("SDL3 is not installed")
	}
	if err != nil {
		t.Fatal(err)
	}
	return r.lib
}

// attachVirtual attaches a virtual device named name and returns its id and
// an open handle the test sets its inputs through.
func attachVirtual(t *testing.T, lib *sdlLib, name string, gamepad bool, axes, buttons, hats uint16) (uint32, uintptr) {
	t.Helper()
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
	id := lib.AttachVirtualJoystick(&desc)
	runtime.KeepAlive(cname)
	if id == 0 {
		t.Fatalf("attach %s: %s", name, lib.GetError())
	}
	js := lib.OpenJoystick(id)
	if js == 0 {
		t.Fatalf("open %s: %s", name, lib.GetError())
	}
	t.Cleanup(func() {
		lib.CloseJoystick(js)
		lib.DetachVirtualJoystick(id)
	})
	return id, js
}

// pump applies SDL's pending device updates to s.
func pump(lib *sdlLib, s *stream) {
	var ev sdlEvent
	for lib.PollEvent(&ev) {
		s.event(&ev)
	}
}

// changed returns the state of id in frame, or nil.
func changed(frame *pb.ControllerFrame, id uint32) *pb.ControllerState {
	for _, st := range frame.GetChanged() {
		if st.GetId() == id {
			return st
		}
	}
	return nil
}

// TestStreamSendsFullThenChanges reads a virtual gamepad and an eight-axis
// virtual joystick: the first frame of an epoch lists both whole, a change
// frame carries only the moved device, a tap between two frames arrives as a
// press and a release, a new epoch lists everything again, and a detached
// device is listed as removed.
func TestStreamSendsFullThenChanges(t *testing.T) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	lib := openSDL(t)
	if !lib.InitSubSystem(sdlInitJoystick | sdlInitGamepad) {
		t.Fatal(lib.GetError())
	}
	defer lib.QuitSubSystem(sdlInitJoystick | sdlInitGamepad)

	// Attach the two devices and open them as the reader does.
	padID, padJS := attachVirtual(t, lib, "modlock test pad", true, 6, 15, 0)
	radioID, radioJS := attachVirtual(t, lib, "modlock test radio", false, 8, 4, 1)
	s := &stream{lib: lib, log: slog.Default(), devices: make(map[uint32]*device)}
	defer s.closeAll()
	s.attach(padID)
	s.attach(radioID)
	pump(lib, s)

	// The first frame of an epoch lists both devices and their states.
	t0 := time.Now()
	full := s.frame(1, t0)
	if !full.GetFull() || full.GetEpoch() != 1 || len(full.GetAdded()) != 2 || len(full.GetChanged()) != 2 {
		t.Fatalf("first frame: %v", full)
	}
	byID := map[uint32]*pb.ControllerInfo{}
	for _, info := range full.GetAdded() {
		byID[info.GetId()] = info
	}
	if pad := byID[padID]; !pad.GetGamepad() || pad.GetAxes() != 6 || pad.GetName() != "modlock test pad" || len(pad.GetGuid()) != 32 {
		t.Fatalf("pad info: %v", pad)
	}
	if radio := byID[radioID]; radio.GetGamepad() || radio.GetAxes() != 8 || radio.GetHats() != 1 {
		t.Fatalf("radio info: %v", radio)
	}

	// Nothing is due before the interval, nor after it with nothing changed.
	if f := s.frame(1, t0.Add(MinInterval/2)); f != nil {
		t.Fatalf("frame inside the interval: %v", f)
	}
	if f := s.frame(1, t0.Add(2*MinInterval)); f != nil {
		t.Fatalf("frame with no change: %v", f)
	}

	// A moved radio channel sends the radio alone.
	lib.SetJoystickVirtualAxis(radioJS, 7, 16384)
	pump(lib, s)
	moved := s.frame(1, t0.Add(3*MinInterval))
	st := changed(moved, radioID)
	if moved.GetFull() || len(moved.GetChanged()) != 1 || st == nil || st.GetAxes()[7] < 0.49 || st.GetAxes()[7] > 0.51 {
		t.Fatalf("moved frame: %v", moved)
	}

	// A tap between two frames arrives as a press and a release, raw and on
	// the gamepad layer, while the button is no longer held.
	for _, down := range []bool{true, false} {
		lib.SetJoystickVirtualButton(padJS, 0, down)
		pump(lib, s)
	}
	tap := changed(s.frame(1, t0.Add(4*MinInterval)), padID)
	if tap == nil || !slices.Equal(tap.GetPressed(), []uint32{0}) || !slices.Equal(tap.GetReleased(), []uint32{0}) ||
		!slices.Equal(tap.GetGamepad().GetPressed(), []pb.GamepadButton{pb.GamepadButton_GAMEPAD_BUTTON_SOUTH}) ||
		!slices.Equal(tap.GetGamepad().GetReleased(), []pb.GamepadButton{pb.GamepadButton_GAMEPAD_BUTTON_SOUTH}) ||
		len(tap.GetGamepad().GetHeld()) != 0 || tap.GetButtons()[0] {
		t.Fatalf("tap: %v", tap)
	}

	// A heartbeat goes with no change once Heartbeat passes.
	if beat := s.frame(1, t0.Add(4*MinInterval+Heartbeat)); beat == nil || len(beat.GetChanged())+len(beat.GetAdded()) != 0 {
		t.Fatalf("heartbeat: %v", beat)
	}

	// A new epoch lists everything again, with the radio's moved channel.
	again := s.frame(2, t0.Add(time.Second))
	if !again.GetFull() || len(again.GetAdded()) != 2 || changed(again, radioID).GetAxes()[7] < 0.49 {
		t.Fatalf("new epoch: %v", again)
	}

	// A detached device is removed in the next frame.
	s.detach(radioID)
	gone := s.frame(2, t0.Add(time.Second+MinInterval))
	if !slices.Equal(gone.GetRemoved(), []uint32{radioID}) || len(gone.GetAdded()) != 0 {
		t.Fatalf("detached: %v", gone)
	}
}

// TestRunFollowsHotplugAndStops runs the reader, sees a device attached
// while it runs announced, and sees Run return once its context ends.
func TestRunFollowsHotplugAndStops(t *testing.T) {
	lib := openSDL(t)
	if !lib.InitSubSystem(sdlInitJoystick | sdlInitGamepad) {
		t.Fatal(lib.GetError())
	}
	t.Cleanup(func() { lib.QuitSubSystem(sdlInitJoystick | sdlInitGamepad) })
	r := &Reader{lib: lib, log: slog.Default()}
	r.Want(1)

	// Run the reader, handing each frame to the test.
	ctx, cancel := context.WithCancel(context.Background())
	frames := make(chan *pb.ControllerFrame, 64)
	done := make(chan error, 1)
	go func() {
		done <- r.Run(ctx, func(f *pb.ControllerFrame) error {
			select {
			case frames <- f:
			case <-ctx.Done():
			}
			return nil
		})
	}()
	next := func() *pb.ControllerFrame {
		select {
		case f := <-frames:
			return f
		case <-time.After(2 * time.Second):
			t.Fatal("no frame")
			return nil
		}
	}
	if first := next(); !first.GetFull() {
		t.Fatalf("first frame: %v", first)
	}

	// A device attached while the reader runs is announced.
	id, _ := attachVirtual(t, lib, "modlock hotplug pad", true, 6, 15, 0)
	deadline := time.Now().Add(2 * time.Second)
	for !slices.ContainsFunc(next().GetAdded(), func(i *pb.ControllerInfo) bool { return i.GetId() == id }) {
		if time.Now().After(deadline) {
			t.Fatal("hotplugged device not announced")
		}
	}

	// Ending the context ends Run.
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Run did not stop")
	}
}

// TestRunStreamsVirtualDevices runs the reader with VirtualEnv set and
// decodes the virtual gamepad's clock axis to within a few frames of now.
func TestRunStreamsVirtualDevices(t *testing.T) {
	lib := openSDL(t)
	t.Setenv(VirtualEnv, "1")
	r := &Reader{lib: lib, log: slog.Default()}
	r.Want(1)

	// Run the reader until the pad's clock axis has been read.
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	names := map[uint32]string{}
	var skew time.Duration
	decoded := false
	err := r.Run(ctx, func(f *pb.ControllerFrame) error {
		for _, info := range f.GetAdded() {
			names[info.GetId()] = info.GetName()
		}
		for _, st := range f.GetChanged() {
			if names[st.GetId()] != "Modlock virtual gamepad" || st.GetGamepad() == nil {
				continue
			}
			// The axis is (t mod 2 s) / 1 s - 1.
			now := time.Now()
			read := (float64(st.GetGamepad().GetLeftX()) + 1) * float64(time.Second)
			skew, decoded = (time.Duration(now.UnixNano()%int64(virtualPeriod))-time.Duration(read)+virtualPeriod)%virtualPeriod, true
			cancel()
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Contains(slices.Collect(maps.Values(names)), "Modlock virtual radio") || !decoded {
		t.Fatalf("devices %v, skew %v", names, skew)
	}
	if skew > MinInterval {
		t.Fatalf("clock axis is %v behind", skew)
	}
}
