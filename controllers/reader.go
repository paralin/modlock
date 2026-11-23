// Package controllers reads the game controllers on a player's PC through
// SDL3 and yields the ControllerFrames a client streams to the game's host,
// as modlock.wasm.controllers describes.
//
// SDL3 is loaded at run time, so a client without it runs on and reports
// ErrUnavailable. SDL opens a device without exclusive access, so Steam Input
// and the game keep reading it, and the reader asks for no enhanced reports
// or lights that would change what other programs see.
package controllers

import (
	"context"
	"errors"
	"log/slog"
	"maps"
	"runtime"
	"slices"
	"sync"
	"sync/atomic"
	"time"

	pb "github.com/paralin/modlock/src/wasm/controllers"
)

// Heartbeat is the longest a running reader goes without sending a frame,
// well inside the host's 200 millisecond stale deadline.
const Heartbeat = 50 * time.Millisecond

// MinInterval is the shortest gap between two frames: 128 a second, two per
// 64 Hz server tick. Presses and releases in between wait for the next one.
const MinInterval = time.Second / 128

// loadOnce loads SDL3 the first time a Reader opens.
var loadOnce = sync.OnceValues(loadSDL)

// Reader streams the controllers attached to this PC. SDL's state is
// process-wide, so one Run goes at a time.
type Reader struct {
	lib *sdlLib
	log *slog.Logger

	// epoch is the want epoch the next frame answers, 0 before any want.
	epoch atomic.Uint32

	// running holds one Run at a time.
	running sync.Mutex
}

// Open loads SDL3, or returns ErrUnavailable when it is not installed. A
// nil log uses slog's default.
func Open(log *slog.Logger) (*Reader, error) {
	lib, err := loadOnce()
	if err != nil {
		return nil, err
	}
	if log == nil {
		log = slog.Default()
	}
	return &Reader{lib: lib, log: log}, nil
}

// Want sets the want epoch the frames answer. A running reader sends a full
// frame for each new epoch.
func (r *Reader) Want(epoch uint32) {
	r.epoch.Store(epoch)
	r.lib.wake()
}

// Run opens every attached device, follows hotplug and focus, and calls send
// with each frame until ctx ends, then closes the devices and returns nil.
// It sends nothing until Want gives an epoch. A send error ends Run with it.
func (r *Reader) Run(ctx context.Context, send func(*pb.ControllerFrame) error) error {
	// SDL reads devices on the thread that started it.
	r.running.Lock()
	defer r.running.Unlock()
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	// Read devices in the background, leaving their reports and lights as
	// other programs set them.
	lib := r.lib
	lib.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1")
	lib.SetHint("SDL_JOYSTICK_ENHANCED_REPORTS", "0")
	lib.SetHint("SDL_JOYSTICK_HIDAPI_PS5_PLAYER_LED", "0")
	lib.SetHint("SDL_JOYSTICK_THREAD", "1")

	// Start SDL's joystick and gamepad layers, and wake them when ctx ends.
	if !lib.InitSubSystem(sdlInitJoystick | sdlInitGamepad) {
		return errors.New("controllers: start SDL: " + lib.GetError())
	}
	defer lib.QuitSubSystem(sdlInitJoystick | sdlInitGamepad)
	stop := context.AfterFunc(ctx, lib.wake)
	defer stop()

	// Open the devices attached now; later ones arrive as events.
	s := &stream{lib: lib, log: r.log, devices: make(map[uint32]*device)}
	defer s.closeAll()
	for _, id := range lib.joysticks() {
		s.attach(id)
	}

	// Attach the virtual devices VirtualEnv asks for; they arrive as events.
	var virtual *virtualDevices
	if wantVirtual() {
		var err error
		if virtual, err = openVirtual(lib); err != nil {
			return err
		}
		defer virtual.detach()
	}

	// Send each frame that is due, then wait for an event or the next one.
	var ev sdlEvent
	for ctx.Err() == nil {
		if virtual != nil {
			// Move the virtual inputs, and apply them before the frame reads.
			virtual.set(time.Now())
			lib.UpdateJoysticks()
			for lib.PollEvent(&ev) {
				s.event(&ev)
			}
		}
		if frame := s.frame(r.epoch.Load(), time.Now()); frame != nil {
			if err := send(frame); err != nil {
				return err
			}
		}
		wait := s.wait(time.Now())
		if virtual != nil {
			// Wake at the frame rate to move the virtual inputs.
			wait = min(wait, int32(MinInterval/time.Millisecond))
		}
		if lib.WaitEventTimeout(&ev, wait) {
			s.event(&ev)
			for lib.PollEvent(&ev) {
				s.event(&ev)
			}
		}
	}
	return nil
}

// stream holds the open devices and what the host has received of them.
type stream struct {
	lib     *sdlLib
	log     *slog.Logger
	focus   focusWatch
	devices map[uint32]*device

	// removed lists the ids of announced devices detached since the last
	// frame.
	removed []uint32

	// epoch and focused are what the last frame carried, last when it went,
	// and dirty whether an input changed since.
	epoch   uint32
	focused bool
	last    time.Time
	dirty   bool
}

// attach opens joystick id unless it is open. A device that fails to open,
// such as one another program holds exclusively, is logged and left out.
func (s *stream) attach(id uint32) {
	// Skip an open device.
	if s.devices[id] != nil {
		return
	}

	// Open it, or log why it cannot be read.
	d, err := openDevice(s.lib, id)
	if err != nil {
		s.log.Warn("controller unavailable", "id", id, "err", err)
		return
	}
	s.devices[id] = d
	s.dirty = true
}

// detach closes joystick id and tells the host if it knew the device.
func (s *stream) detach(id uint32) {
	// Skip a device that is not open.
	d := s.devices[id]
	if d == nil {
		return
	}

	// Close it, and list it as removed if a frame announced it.
	d.close(s.lib)
	delete(s.devices, id)
	if d.announced {
		s.removed = append(s.removed, id)
	}
	s.dirty = true
}

// closeAll closes every device.
func (s *stream) closeAll() {
	for _, d := range s.devices {
		d.close(s.lib)
	}
	clear(s.devices)
}

// event applies one SDL event: hotplug, a button edge, or any input change.
func (s *stream) event(ev *sdlEvent) {
	kind := ev.kind()
	switch kind {
	case sdlEventJoystickAdded:
		s.attach(ev.which())
	case sdlEventJoystickRemoved:
		s.detach(ev.which())
	case sdlEventJoystickButtonDown, sdlEventJoystickButtonUp,
		sdlEventGamepadButtonDown, sdlEventGamepadButtonUp:
		if d := s.devices[ev.which()]; d != nil && s.focused {
			pad := kind == sdlEventGamepadButtonDown || kind == sdlEventGamepadButtonUp
			down := kind == sdlEventJoystickButtonDown || kind == sdlEventGamepadButtonDown
			d.edge(ev.button(), down, pad)
		}
		s.dirty = true
	default:
		if kind >= 0x600 && kind < 0x700 {
			s.dirty = true
		}
	}
}

// wait returns how many milliseconds to wait for an event before the next
// frame is due: MinInterval after the last one once an input changed, and
// Heartbeat otherwise.
func (s *stream) wait(now time.Time) int32 {
	due := s.last.Add(Heartbeat)
	if s.dirty {
		due = s.last.Add(MinInterval)
	}
	return int32(max(due.Sub(now)+time.Millisecond-1, 0) / time.Millisecond)
}

// frame returns the frame due at now for epoch, or nil when none is: the
// whole set for a new epoch, and otherwise what changed, at most every
// MinInterval, or an empty heartbeat after Heartbeat.
func (s *stream) frame(epoch uint32, now time.Time) *pb.ControllerFrame {
	// Decide whether a frame is due.
	if epoch == 0 {
		return nil
	}
	full := epoch != s.epoch
	focused := s.focus.Focused()
	if !full {
		if now.Sub(s.last) < MinInterval {
			return nil
		}
		if !s.dirty && focused == s.focused && now.Sub(s.last) < Heartbeat {
			return nil
		}
	}

	// A full frame lists every device; a change frame the removed ones.
	frame := &pb.ControllerFrame{Epoch: epoch, Full: full, Focused: focused}
	if !full {
		frame.Removed = s.removed
	}
	s.removed = nil

	// List each new device, and each state that changed or has edges.
	for _, id := range slices.Sorted(maps.Keys(s.devices)) {
		d := s.devices[id]
		if full {
			d.announced, d.sent = false, nil
			d.clearEdges()
		}
		if !d.announced {
			frame.Added = append(frame.Added, d.info)
			d.announced = true
		}
		st := d.read(s.lib, focused)
		if d.sent != nil && !d.hasEdges() && st.EqualVT(d.sent) {
			continue
		}
		d.sent = st
		if focused && d.hasEdges() {
			st = d.withEdges(st)
		}
		d.clearEdges()
		frame.Changed = append(frame.Changed, st)
	}
	s.epoch, s.focused, s.last, s.dirty = epoch, focused, now, false
	return frame
}
