package sandbox

import (
	"github.com/paralin/modlock/src/geometry"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/movement"
)

// answerWorldCall handles Teleport, MovePlayer and WatchMovement. It returns
// nil for a method this file does not answer.
func (s *Sandbox) answerWorldCall(call *wasm.Call) *wasm.Reply {
	switch call.GetMethod() {
	case "Teleport":
		request := &movement.TeleportRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the Teleport request is malformed"}
		}
		return s.teleport(request)
	case "MovePlayer":
		request := &movement.MovePlayerRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the MovePlayer request is malformed"}
		}
		return s.movePlayer(request)
	case "WatchMovement":
		request := &movement.WatchMovementRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the WatchMovement request is malformed"}
		}
		return s.watchMovement(request)
	default:
		return nil
	}
}

// teleport places the stand-in pawn at once, and turns its aim and camera, as
// the host's Teleport does.
func (s *Sandbox) teleport(request *movement.TeleportRequest) *wasm.Reply {
	s.ensureRoster()
	if request == nil {
		return &wasm.Reply{Error: "the Teleport request is malformed"}
	}
	occupied, err := s.liveSeat(request.GetPlayer())
	if err != "" {
		return &wasm.Reply{Error: err}
	}
	s.placePawn(occupied, request.GetPosition(), request.GetFacing(), request.Velocity, true)
	return &wasm.Reply{}
}

// movePlayer sets the stand-in pawn's pose for this step. The host applies
// MovePlayer immediately, one tick at a time; a recording such as Puppet
// interpolates by calling it again each frame. The sandbox does the same,
// rather than easing toward the target itself.
func (s *Sandbox) movePlayer(request *movement.MovePlayerRequest) *wasm.Reply {
	s.ensureRoster()
	if request == nil {
		return &wasm.Reply{Error: "the MovePlayer request is malformed"}
	}
	occupied, err := s.liveSeat(request.GetPlayer())
	if err != "" {
		return &wasm.Reply{Error: err}
	}
	s.placePawn(occupied, request.GetPosition(), request.GetFacing(), request.Velocity, false)
	return &wasm.Reply{}
}

// watchMovement starts or stops movement samples for one slot on later Frame
// events, as the host's WatchMovement does.
func (s *Sandbox) watchMovement(request *movement.WatchMovementRequest) *wasm.Reply {
	s.ensureRoster()
	if request == nil {
		return &wasm.Reply{Error: "the WatchMovement request is malformed"}
	}
	slot := request.GetPlayer()
	if slot < 0 || slot >= maxPlayerSlots {
		return &wasm.Reply{Error: "no player has that slot"}
	}
	if s.watched == nil {
		s.watched = map[int32]bool{}
	}
	if request.GetWatch() {
		s.watched[slot] = true
	} else {
		delete(s.watched, slot)
	}
	return &wasm.Reply{}
}

// frameEvent is the Frame the sandbox delivers: the tick, the elapsed time,
// and a movement sample for each watched, living stand-in pawn.
func (s *Sandbox) frameEvent(elapsed float64) *wasm.FrameEvent {
	return &wasm.FrameEvent{
		Tick:        s.tick,
		TimeSeconds: elapsed,
		Movement:    s.movementSamples(),
	}
}

// movementSamples lists the current pose of every watched, living pawn, in
// slot order, as the host attaches them to a Frame that starts a tick.
func (s *Sandbox) movementSamples() []*movement.MovementSample {
	s.ensureRoster()
	if len(s.watched) == 0 {
		return nil
	}
	var samples []*movement.MovementSample
	for slot := int32(0); slot < maxPlayerSlots; slot++ {
		if !s.watched[slot] {
			continue
		}
		occupied := s.seats[slot]
		if occupied == nil || occupied.pawn == nil || occupied.pawn.GetHealth() <= 0 {
			continue
		}
		pawn := occupied.pawn
		samples = append(samples, &movement.MovementSample{
			Player:   slot,
			Pawn:     pawn.GetEntity(),
			Position: copyVec3(pawn.GetPosition()),
			Velocity: copyVec3(pawn.GetVelocity()),
			Grounded: true,
		})
	}
	return samples
}

// liveSeat returns the occupied seat of a living pawn, or why the call fails.
func (s *Sandbox) liveSeat(slot int32) (*seat, string) {
	s.ensureRoster()
	occupied, ok := s.seats[slot]
	if !ok {
		return nil, "no such player"
	}
	if occupied.pawn == nil || occupied.pawn.GetHealth() <= 0 {
		return nil, "the player has no hero"
	}
	return occupied, ""
}

// placePawn writes a pose onto the stand-in pawn. Teleport also turns the
// camera; MovePlayer leaves the camera alone and aims the eyes at facing.
func (s *Sandbox) placePawn(occupied *seat, position *geometry.Vec3, facing *geometry.EulerAngles, velocity *geometry.Vec3, turnCamera bool) {
	origin := copyVec3(position)
	look := copyAngles(facing)
	occupied.position = origin
	pawn := occupied.pawn
	pawn.Position = origin
	pawn.EyeAngles = look
	pawn.EyePosition = &geometry.Vec3{X: origin.X, Y: origin.Y, Z: origin.Z + 64}
	if velocity != nil {
		pawn.Velocity = copyVec3(velocity)
	} else if turnCamera {
		pawn.Velocity = &geometry.Vec3{}
	}
	if turnCamera {
		pawn.CameraAngles = copyAngles(facing)
	}
}

func copyAngles(a *geometry.EulerAngles) *geometry.EulerAngles {
	if a == nil {
		return &geometry.EulerAngles{}
	}
	copied := *a
	return &copied
}
