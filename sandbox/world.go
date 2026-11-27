package sandbox

import (
	"github.com/paralin/modlock/src/geometry"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/movement"
	"github.com/paralin/modlock/src/wasm/players"
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

// teleport places the stand-in pawn at once and turns its aim and camera, as
// the host's Teleport does. Like the game's teleport, it keeps the pawn's
// velocity unless the request sets one.
func (s *Sandbox) teleport(request *movement.TeleportRequest) *wasm.Reply {
	// Place the living pawn, then turn its camera.
	pawn, err := s.livePawn(request.GetPlayer())
	if err != "" {
		return &wasm.Reply{Error: err}
	}
	velocity := pawn.GetVelocity()
	if request.Velocity != nil {
		velocity = request.GetVelocity()
	}
	s.place(request.GetPlayer(), request.GetPosition(), request.GetFacing(), velocity)
	pawn.CameraAngles = copyAngles(request.GetFacing())
	return &wasm.Reply{}
}

// movePlayer sets the stand-in pawn's pose for this step, as the host's
// MovePlayer does: at once, with the velocity the request gives or none, and
// the camera left alone. A recording interpolates by calling it each frame.
func (s *Sandbox) movePlayer(request *movement.MovePlayerRequest) *wasm.Reply {
	if _, err := s.livePawn(request.GetPlayer()); err != "" {
		return &wasm.Reply{Error: err}
	}
	s.place(request.GetPlayer(), request.GetPosition(), request.GetFacing(), request.GetVelocity())
	return &wasm.Reply{}
}

// watchMovement starts or stops movement samples for one slot on later
// Frame events, as the host's WatchMovement does.
func (s *Sandbox) watchMovement(request *movement.WatchMovementRequest) *wasm.Reply {
	if !s.roster.watch(request.GetPlayer(), request.GetWatch()) {
		return &wasm.Reply{Error: "no player has that slot"}
	}
	return &wasm.Reply{}
}

// frameEvent is the Frame the sandbox delivers: the tick, the elapsed time,
// and a movement sample of each watched, living stand-in pawn in slot order.
func (s *Sandbox) frameEvent(elapsed float64) *wasm.FrameEvent {
	event := &wasm.FrameEvent{Tick: s.tick, TimeSeconds: elapsed}
	for slot, occupied := range s.roster.occupied() {
		if !s.roster.watching(slot) || occupied.pawn.GetHealth() <= 0 {
			continue
		}
		pawn := occupied.pawn
		event.Movement = append(event.Movement, &movement.MovementSample{
			Player:   slot,
			Pawn:     pawn.GetEntity(),
			Position: copyVec3(pawn.GetPosition()),
			Velocity: copyVec3(pawn.GetVelocity()),
			Grounded: true,
		})
	}
	return event
}

// livePawn returns the living stand-in pawn in slot, or the host's reason
// there is none.
func (s *Sandbox) livePawn(slot int32) (*players.Pawn, string) {
	occupied := s.roster.seat(slot)
	if occupied == nil {
		return nil, "no player is in that slot"
	}
	if occupied.pawn.GetHealth() <= 0 {
		return nil, "the player has no live hero"
	}
	return occupied.pawn, ""
}

// place stands slot's living pawn at position, aimed at facing and moving at
// velocity, and keeps the position for the seat's next pawn.
func (s *Sandbox) place(slot int32, position *geometry.Vec3, facing *geometry.EulerAngles, velocity *geometry.Vec3) {
	// Keep the position for the seat, then pose the pawn.
	occupied := s.roster.seat(slot)
	occupied.position = copyVec3(position)
	pawn := occupied.pawn
	pawn.Position = copyVec3(position)
	pawn.EyePosition = eyePosition(pawn.Position)
	pawn.EyeAngles = copyAngles(facing)
	pawn.Velocity = copyVec3(velocity)
}

// copyAngles copies a, reading missing angles as facing ahead.
func copyAngles(a *geometry.EulerAngles) *geometry.EulerAngles {
	if a == nil {
		return &geometry.EulerAngles{}
	}
	return a.CloneVT()
}
