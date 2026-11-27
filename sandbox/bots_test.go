package sandbox

import (
	"testing"

	"github.com/paralin/modlock/src/geometry"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/players"
)

// TestBots covers the sandbox roster and stand-in pawns: AddBot fills slots
// from 1, Players lists them with the human, RemoveBot frees a slot for a
// new generation, and Pawn, SelectHero, ChangeTeam, Spectate, Kill and
// Respawn update stored state without a game.
func TestBots(t *testing.T) {
	s := newSandbox(Config{})
	mod := testBuild("hud")

	// Add two bots; they take slots 1 and 2 and keep the options they were
	// given. The human stays in slot 0.
	first := mustAddBot(t, s, mod, &players.BotOptions{
		Name:     "Abrams",
		Team:     3,
		Hero:     &players.BotOptions_HeroId{HeroId: 4},
		Position: &geometry.Vec3{X: 1, Y: 2, Z: 3},
	})
	if first != 1 {
		t.Fatalf("first bot slot = %d, want 1", first)
	}
	second := mustAddBot(t, s, mod, &players.BotOptions{
		Name: "",
		Team: 2,
		Hero: &players.BotOptions_HeroId{HeroId: 7},
	})
	if second != 2 {
		t.Fatalf("second bot slot = %d, want 2", second)
	}

	conns := mustPlayers(t, s, mod)
	if len(conns) != 3 {
		t.Fatalf("Players listed %d, want 3", len(conns))
	}
	if conns[0].GetPlayer() != 0 || conns[0].GetBot() || conns[0].GetSteamId() == 0 {
		t.Fatalf("slot 0 = %+v, want the stand-in human", conns[0])
	}
	if conns[1].GetPlayer() != 1 || !conns[1].GetBot() || conns[1].GetSteamId() != 0 || !conns[1].GetReady() || conns[1].GetName() != "Abrams" || conns[1].GetGeneration() != 1 {
		t.Fatalf("slot 1 = %+v, want a ready bot named Abrams", conns[1])
	}
	if conns[2].GetName() != "Bot 2" {
		t.Fatalf("an unnamed bot was named %q, want Bot 2", conns[2].GetName())
	}

	pawn := mustPawn(t, s, mod, 1)
	if pawn.GetHero() != 4 || pawn.GetTeam() != 3 || pawn.GetHealth() <= 0 || pawn.GetEntity() == 0 {
		t.Fatalf("bot pawn = %+v, want hero 4 on team 3", pawn)
	}
	if pawn.GetPosition().GetX() != 1 || pawn.GetPosition().GetY() != 2 || pawn.GetPosition().GetZ() != 3 {
		t.Fatalf("bot position = %+v, want 1,2,3", pawn.GetPosition())
	}
	pawns := mustPawns(t, s, mod)
	if len(pawns) != 3 {
		t.Fatalf("Pawns listed %d, want 3", len(pawns))
	}

	// RemoveBot only drops a bot this mod added, then a new bot reuses the
	// slot with a new generation.
	if err := callPlayer(s, mod, "RemoveBot", 0).GetError(); err == "" {
		t.Fatal("RemoveBot on the human was accepted")
	}
	other := testBuild("other")
	if err := callPlayer(s, other, "RemoveBot", 1).GetError(); err == "" {
		t.Fatal("RemoveBot from another mod was accepted")
	}
	if err := callPlayer(s, mod, "RemoveBot", 1).GetError(); err != "" {
		t.Fatalf("RemoveBot 1: %s", err)
	}
	conns = mustPlayers(t, s, mod)
	if len(conns) != 2 || conns[1].GetPlayer() != 2 {
		t.Fatalf("after RemoveBot, Players = %v", slotsOf(conns))
	}
	reused := mustAddBot(t, s, mod, &players.BotOptions{
		Name: "Viscous",
		Team: 3,
		Hero: &players.BotOptions_HeroId{HeroId: 11},
	})
	if reused != 1 {
		t.Fatalf("reused slot = %d, want 1", reused)
	}
	conns = mustPlayers(t, s, mod)
	if conns[1].GetGeneration() != 2 || conns[1].GetName() != "Viscous" {
		t.Fatalf("reused seat = %+v, want generation 2 named Viscous", conns[1])
	}

	// A bot keeps the hero AddBot gave it; the human can select a hero,
	// change team, be killed and respawn, or spectate.
	if err := callSelect(s, mod, 1, 9, 2).GetError(); err == "" {
		t.Fatal("SelectHero on a bot was accepted")
	}
	if err := callSelect(s, mod, 0, 15, 2).GetError(); err != "" {
		t.Fatalf("SelectHero on the human: %s", err)
	}
	human := mustPawn(t, s, mod, 0)
	if human.GetHero() != 15 || human.GetTeam() != 2 {
		t.Fatalf("human pawn after SelectHero = %+v, want hero 15 team 2", human)
	}
	if err := callChangeTeam(s, mod, 1, 2).GetError(); err != "" {
		t.Fatalf("ChangeTeam on a bot: %s", err)
	}
	if mustPawn(t, s, mod, 1).GetTeam() != 2 {
		t.Fatal("ChangeTeam did not update the bot pawn")
	}
	if err := callPlayer(s, mod, "Kill", 1).GetError(); err != "" {
		t.Fatalf("Kill: %s", err)
	}
	if mustPawn(t, s, mod, 1).GetHealth() != 0 {
		t.Fatal("Kill did not zero the bot's health")
	}
	if err := callPlayer(s, mod, "Respawn", 1).GetError(); err != "" {
		t.Fatalf("Respawn: %s", err)
	}
	revived := mustPawn(t, s, mod, 1)
	if revived.GetHealth() != defaultMaxHealth || revived.GetHero() != 11 {
		t.Fatalf("respawned pawn = %+v, want full health and hero 11", revived)
	}
	if err := callPlayer(s, mod, "Spectate", 1).GetError(); err != "" {
		t.Fatalf("Spectate: %s", err)
	}
	if mustPawn(t, s, mod, 1) != nil {
		t.Fatal("Spectate left a pawn")
	}
	if err := callPlayer(s, mod, "Respawn", 1).GetError(); err != "" {
		t.Fatalf("Respawn after spectate: %s", err)
	}
	if mustPawn(t, s, mod, 1) == nil || mustPawn(t, s, mod, 1).GetHero() != 11 {
		t.Fatal("Respawn after spectate did not restore the bot's hero")
	}

	// A name the sandbox cannot resolve is refused, as is a missing hero.
	if _, err := addBot(s, mod, &players.BotOptions{
		Name: "Named",
		Hero: &players.BotOptions_HeroName{HeroName: "hero_wraith"},
	}); err == "" {
		t.Fatal("AddBot with an unresolvable hero name was accepted")
	}
	if _, err := addBot(s, mod, &players.BotOptions{Name: "Empty"}); err == "" {
		t.Fatal("AddBot without a hero was accepted")
	}

	// Bots a mod added leave when it stops, and a failed reload keeps them.
	held := s.roster.take("hud")
	if len(mustPlayers(t, s, mod)) != 1 {
		t.Fatalf("after take, Players listed %d, want only the human", len(mustPlayers(t, s, mod)))
	}
	s.roster.restore(held)
	if got := slotsOf(mustPlayers(t, s, mod)); len(got) != 3 {
		t.Fatalf("restore listed %v, want the two bots back", got)
	}
	s.roster.take("hud")
	if len(mustPlayers(t, s, mod)) != 1 {
		t.Fatal("take left bots in the roster")
	}
}

func testBuild(slug string) *Build {
	return &Build{Manifest: &wasm.Manifest{Slug: slug}}
}

func addBot(s *Sandbox, build *Build, options *players.BotOptions) (int32, string) {
	request, _ := options.MarshalVT()
	reply := s.answer(build, &serviceCalls{}, &wasm.Call{Method: "AddBot", Request: request})
	if err := reply.GetError(); err != "" {
		return 0, err
	}
	response := &players.BotResponse{}
	if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
		return 0, err.Error()
	}
	return response.GetPlayer(), ""
}

func mustAddBot(t *testing.T, s *Sandbox, build *Build, options *players.BotOptions) int32 {
	t.Helper()
	slot, err := addBot(s, build, options)
	if err != "" {
		t.Fatalf("AddBot: %s", err)
	}
	return slot
}

func mustPlayers(t *testing.T, s *Sandbox, build *Build) []*players.Connection {
	t.Helper()
	reply := s.answer(build, &serviceCalls{}, &wasm.Call{Method: "Players"})
	if err := reply.GetError(); err != "" {
		t.Fatalf("Players: %s", err)
	}
	response := &players.PlayersResponse{}
	if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
		t.Fatal(err)
	}
	return response.GetPlayers()
}

func mustPawn(t *testing.T, s *Sandbox, build *Build, slot int32) *players.Pawn {
	t.Helper()
	reply := callPlayer(s, build, "Pawn", slot)
	if err := reply.GetError(); err != "" {
		t.Fatalf("Pawn %d: %s", slot, err)
	}
	response := &players.PawnResponse{}
	if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
		t.Fatal(err)
	}
	return response.GetPawn()
}

func mustPawns(t *testing.T, s *Sandbox, build *Build) []*players.PlayerPawn {
	t.Helper()
	reply := s.answer(build, &serviceCalls{}, &wasm.Call{Method: "Pawns"})
	if err := reply.GetError(); err != "" {
		t.Fatalf("Pawns: %s", err)
	}
	response := &players.PawnsResponse{}
	if err := response.UnmarshalVT(reply.GetResponse()); err != nil {
		t.Fatal(err)
	}
	return response.GetPawns()
}

func callPlayer(s *Sandbox, build *Build, method string, slot int32) *wasm.Reply {
	request, _ := (&players.PlayerRequest{Player: slot}).MarshalVT()
	return s.answer(build, &serviceCalls{}, &wasm.Call{Method: method, Request: request})
}

func callSelect(s *Sandbox, build *Build, slot int32, hero uint32, team int32) *wasm.Reply {
	request, _ := (&players.SelectHeroRequest{
		Player: slot,
		Hero:   &players.SelectHeroRequest_HeroId{HeroId: hero},
		Team:   team,
	}).MarshalVT()
	return s.answer(build, &serviceCalls{}, &wasm.Call{Method: "SelectHero", Request: request})
}

func callChangeTeam(s *Sandbox, build *Build, slot, team int32) *wasm.Reply {
	request, _ := (&players.ChangeTeamRequest{Player: slot, Team: team}).MarshalVT()
	return s.answer(build, &serviceCalls{}, &wasm.Call{Method: "ChangeTeam", Request: request})
}

func slotsOf(conns []*players.Connection) []int32 {
	slots := make([]int32, len(conns))
	for i, conn := range conns {
		slots[i] = conn.GetPlayer()
	}
	return slots
}
