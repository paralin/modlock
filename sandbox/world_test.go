package sandbox

import (
	"testing"

	"github.com/paralin/modlock/src/geometry"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/movement"
	"github.com/paralin/modlock/src/wasm/players"
)

// TestWorld covers the sandbox stand-in world: Teleport places a pawn at
// once, MovePlayer snaps each step the way the host does, WatchMovement
// lists only living watched pawns on a Frame, and a spectator or a dead
// hero is left out.
func TestWorld(t *testing.T) {
	s := newSandbox(Config{})
	mod := testBuild("hud")

	// The stand-in human starts at the origin; Teleport places them and
	// turns the camera, and Pawn reads the new pose.
	if err := callWorld(s, mod, "Teleport", 0, &geometry.Vec3{X: 10, Y: 20, Z: 30}, &geometry.EulerAngles{Yaw: 90}, &geometry.Vec3{X: 1}).GetError(); err != "" {
		t.Fatalf("Teleport: %s", err)
	}
	pawn := mustPawn(t, s, mod, 0)
	if pawn.GetPosition().GetX() != 10 || pawn.GetPosition().GetY() != 20 || pawn.GetPosition().GetZ() != 30 {
		t.Fatalf("teleported position = %+v, want 10,20,30", pawn.GetPosition())
	}
	if pawn.GetEyeAngles().GetYaw() != 90 || pawn.GetCameraAngles().GetYaw() != 90 {
		t.Fatalf("teleported angles eye=%+v camera=%+v, want yaw 90", pawn.GetEyeAngles(), pawn.GetCameraAngles())
	}
	if pawn.GetVelocity().GetX() != 1 {
		t.Fatalf("teleported velocity = %+v, want x 1", pawn.GetVelocity())
	}

	// MovePlayer snaps this step and leaves the camera where Teleport turned it.
	if err := callWorld(s, mod, "MovePlayer", 0, &geometry.Vec3{X: 40, Y: 50, Z: 0}, &geometry.EulerAngles{Yaw: 180, Pitch: 10}, &geometry.Vec3{Y: 3}).GetError(); err != "" {
		t.Fatalf("MovePlayer: %s", err)
	}
	pawn = mustPawn(t, s, mod, 0)
	if pawn.GetPosition().GetX() != 40 || pawn.GetPosition().GetY() != 50 {
		t.Fatalf("moved position = %+v, want 40,50,0", pawn.GetPosition())
	}
	if pawn.GetEyeAngles().GetYaw() != 180 || pawn.GetEyeAngles().GetPitch() != 10 {
		t.Fatalf("moved eye angles = %+v, want yaw 180 pitch 10", pawn.GetEyeAngles())
	}
	if pawn.GetCameraAngles().GetYaw() != 90 {
		t.Fatalf("MovePlayer turned the camera to %+v, want yaw 90", pawn.GetCameraAngles())
	}
	if pawn.GetVelocity().GetY() != 3 {
		t.Fatalf("moved velocity = %+v, want y 3", pawn.GetVelocity())
	}

	// A Teleport without a velocity keeps the pawn moving as it was, as the
	// game's teleport does.
	if err := callWorld(s, mod, "Teleport", 0, &geometry.Vec3{X: 40, Y: 50}, &geometry.EulerAngles{Yaw: 90}, nil).GetError(); err != "" {
		t.Fatalf("Teleport: %s", err)
	}
	if pawn = mustPawn(t, s, mod, 0); pawn.GetVelocity().GetY() != 3 {
		t.Fatalf("Teleport without a velocity left %+v, want y 3", pawn.GetVelocity())
	}

	// Frames carry no samples until WatchMovement is on, then one for the
	// living watched pawn.
	if got := s.frameEvent(0).GetMovement(); len(got) != 0 {
		t.Fatalf("unwatched samples = %d, want none", len(got))
	}
	if err := callWatch(s, mod, 0, true).GetError(); err != "" {
		t.Fatalf("WatchMovement on: %s", err)
	}
	samples := s.frameEvent(0).GetMovement()
	if len(samples) != 1 || samples[0].GetPlayer() != 0 || samples[0].GetPosition().GetX() != 40 || !samples[0].GetGrounded() || samples[0].GetPawn() == 0 {
		t.Fatalf("watched samples = %+v, want slot 0 at 40,50", samples)
	}
	if err := callWatch(s, mod, 0, false).GetError(); err != "" {
		t.Fatalf("WatchMovement off: %s", err)
	}
	if got := s.frameEvent(0).GetMovement(); len(got) != 0 {
		t.Fatalf("after unwatch, samples = %d, want none", len(got))
	}

	// A missing slot and a spectator or dead hero are refused; a dead or
	// spectating pawn is omitted from samples even while watched.
	if err := callWorld(s, mod, "Teleport", 7, &geometry.Vec3{}, &geometry.EulerAngles{}, nil).GetError(); err == "" {
		t.Fatal("Teleport of a missing slot was accepted")
	}
	if err := callWatch(s, mod, 99, true).GetError(); err == "" {
		t.Fatal("WatchMovement of an invalid slot was accepted")
	}
	if err := callPlayer(s, mod, "Spectate", 0).GetError(); err != "" {
		t.Fatalf("Spectate: %s", err)
	}
	if err := callWorld(s, mod, "MovePlayer", 0, &geometry.Vec3{X: 1}, &geometry.EulerAngles{}, nil).GetError(); err == "" {
		t.Fatal("MovePlayer on a spectator was accepted")
	}
	if err := callWatch(s, mod, 0, true).GetError(); err != "" {
		t.Fatalf("WatchMovement on spectator: %s", err)
	}
	if got := s.frameEvent(0).GetMovement(); len(got) != 0 {
		t.Fatalf("spectator samples = %d, want none", len(got))
	}

	// A bot can be teleported and watched alongside the human after they
	// respawn with their stored hero.
	if err := callSelect(s, mod, 0, 15, 2).GetError(); err != "" {
		t.Fatalf("SelectHero after spectate: %s", err)
	}
	bot := mustAddBot(t, s, mod, &players.BotOptions{
		Name:     "Abrams",
		Team:     3,
		Hero:     &players.BotOptions_HeroId{HeroId: 4},
		Position: &geometry.Vec3{X: 5, Y: 6, Z: 7},
	})
	if err := callWatch(s, mod, bot, true).GetError(); err != "" {
		t.Fatalf("WatchMovement bot: %s", err)
	}
	if err := callWatch(s, mod, 0, true).GetError(); err != "" {
		t.Fatalf("WatchMovement human: %s", err)
	}
	if got := s.frameEvent(0).GetMovement(); len(got) != 2 || got[0].GetPlayer() != 0 || got[1].GetPlayer() != bot {
		t.Fatalf("two watched samples = %v", slotsOfSamples(got))
	}
	if err := callPlayer(s, mod, "Kill", bot).GetError(); err != "" {
		t.Fatalf("Kill bot: %s", err)
	}
	if got := s.frameEvent(0).GetMovement(); len(got) != 1 || got[0].GetPlayer() != 0 {
		t.Fatalf("after killing the bot, samples = %v, want only the human", slotsOfSamples(got))
	}

	s.tick = 9
	event := s.frameEvent(1.5)
	if event.GetTick() != 9 || event.GetTimeSeconds() != 1.5 || len(event.GetMovement()) != 1 {
		t.Fatalf("frame event = tick %d time %v samples %d", event.GetTick(), event.GetTimeSeconds(), len(event.GetMovement()))
	}
}

func callWorld(s *Sandbox, build *Build, method string, slot int32, position *geometry.Vec3, facing *geometry.EulerAngles, velocity *geometry.Vec3) *wasm.Reply {
	var request []byte
	switch method {
	case "Teleport":
		msg := &movement.TeleportRequest{Player: slot, Position: position, Facing: facing}
		if velocity != nil {
			msg.Velocity = velocity
		}
		request, _ = msg.MarshalVT()
	case "MovePlayer":
		msg := &movement.MovePlayerRequest{Player: slot, Position: position, Facing: facing}
		if velocity != nil {
			msg.Velocity = velocity
		}
		request, _ = msg.MarshalVT()
	}
	return s.answer(build, &serviceCalls{}, &wasm.Call{Method: method, Request: request})
}

func callWatch(s *Sandbox, build *Build, slot int32, watch bool) *wasm.Reply {
	request, _ := (&movement.WatchMovementRequest{Player: slot, Watch: watch}).MarshalVT()
	return s.answer(build, &serviceCalls{}, &wasm.Call{Method: "WatchMovement", Request: request})
}

func slotsOfSamples(samples []*movement.MovementSample) []int32 {
	slots := make([]int32, len(samples))
	for i, sample := range samples {
		slots[i] = sample.GetPlayer()
	}
	return slots
}
