package sandbox

import (
	"context"
	"iter"
	"strconv"

	"github.com/paralin/modlock/src/geometry"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/players"
)

// maxPlayerSlots is the number of slots the sandbox seats, human included.
const maxPlayerSlots = 16

// firstBotSlot is the first slot AddBot may give; slot 0 is the stand-in human.
const firstBotSlot = 1

// firstEntity is the first stand-in pawn handle, so a missing pawn stays 0.
const firstEntity = 1000

// defaultMaxHealth is the health a stand-in pawn starts and respawns with.
const defaultMaxHealth int32 = 100

// eyeHeight is how far above a stand-in pawn's feet its view starts.
const eyeHeight = 64

// roster is the session's seats: the stand-in human in slot 0 and the bots
// mods add, each with the stand-in pawn the pawn and movement calls read.
// One goroutine, the session's, owns it.
type roster struct {
	// seats holds each occupied slot, or nil for an empty one.
	seats [maxPlayerSlots]*seat
	// generations is the last generation each slot handed out, so a slot a
	// new bot reuses gets a new one.
	generations [maxPlayerSlots]uint32
	// watched marks the slots WatchMovement turned on, whose living pawns a
	// Frame samples.
	watched [maxPlayerSlots]bool
	// nextEntity is the next stand-in pawn handle.
	nextEntity uint32
	// revision counts changes to who is seated, so each running mod hears a
	// change once.
	revision uint64
}

// seat is one occupied slot: the stand-in human or a bot a mod added, with
// the state its stand-in pawn is built from.
type seat struct {
	// conn is how Players lists this slot.
	conn *players.Connection
	// pawn is the slot's stand-in pawn, or nil while they spectate.
	pawn *players.Pawn
	// addedBy is the slug of the mod that called AddBot; empty for the human.
	addedBy string
	// hero is the hero SelectHero or AddBot gave, kept across spectate; 0
	// before the human picks one.
	hero uint32
	// team is the team AddBot, SelectHero or ChangeTeam gave.
	team int32
	// position is where the stand-in pawn stands, kept across spectate.
	position *geometry.Vec3
}

// newRoster seats the stand-in human, whose pawn is the game's placeholder
// until SelectHero gives them a hero.
func newRoster() *roster {
	// Seat the human in slot 0 at the roster's first revision.
	r := &roster{nextEntity: firstEntity, revision: 1}
	human := &seat{conn: player.CloneVT(), position: &geometry.Vec3{}}
	human.pawn = r.newPawn(human)
	r.seats[0] = human
	r.generations[0] = player.GetGeneration()
	return r
}

// seat returns the occupied seat in slot, or nil for an empty slot or one
// past the roster.
func (r *roster) seat(slot int32) *seat {
	if slot < 0 || slot >= maxPlayerSlots {
		return nil
	}
	return r.seats[slot]
}

// connections lists every occupied seat in slot order, human first.
func (r *roster) connections() []*players.Connection {
	var list []*players.Connection
	for _, occupied := range r.seats {
		if occupied != nil {
			list = append(list, occupied.conn.CloneVT())
		}
	}
	return list
}

// occupied yields each occupied seat with its slot, in slot order.
func (r *roster) occupied() iter.Seq2[int32, *seat] {
	return func(yield func(int32, *seat) bool) {
		for slot, occupied := range r.seats {
			if occupied != nil && !yield(int32(slot), occupied) {
				return
			}
		}
	}
}

// add seats a bot in the lowest free bot slot under that slot's next
// generation, names it after the slot when it has no name, gives it a pawn,
// and returns the slot, or false when every slot is taken.
func (r *roster) add(bot *seat) (int32, bool) {
	// Find the slot.
	slot := int32(-1)
	for free := int32(firstBotSlot); free < maxPlayerSlots; free++ {
		if r.seats[free] == nil {
			slot = free
			break
		}
	}
	if slot < 0 {
		return 0, false
	}

	// Take the slot's next generation, skipping 0, which names no
	// connection.
	r.generations[slot]++
	if r.generations[slot] == 0 {
		r.generations[slot] = 1
	}

	// Seat the bot.
	bot.conn.Player, bot.conn.Generation = slot, r.generations[slot]
	if bot.conn.Name == "" {
		bot.conn.Name = "Bot " + strconv.Itoa(int(slot))
	}
	bot.pawn = r.newPawn(bot)
	r.seats[slot] = bot
	r.revision++
	return slot, true
}

// remove empties slot.
func (r *roster) remove(slot int32) {
	r.seats[slot] = nil
	r.revision++
}

// take removes the bots mod added and returns them, so a reload whose new
// instance fails can put them back with restore.
func (r *roster) take(mod string) []*seat {
	var taken []*seat
	for slot, occupied := range r.occupied() {
		if occupied.addedBy == mod {
			taken = append(taken, occupied)
			r.remove(slot)
		}
	}
	return taken
}

// restore puts back the seats take removed. Their slots are still free: only
// the mod that took them could have filled them, and its failed instance's
// bots were taken in turn.
func (r *roster) restore(taken []*seat) {
	for _, occupied := range taken {
		r.seats[occupied.conn.GetPlayer()] = occupied
		r.revision++
	}
}

// watch turns movement samples for slot on or off, or reports false for a
// slot past the roster.
func (r *roster) watch(slot int32, on bool) bool {
	if slot < 0 || slot >= maxPlayerSlots {
		return false
	}
	r.watched[slot] = on
	return true
}

// watching reports whether WatchMovement turned on slot's samples.
func (r *roster) watching(slot int32) bool {
	return r.watched[slot]
}

// newPawn builds a full-health stand-in pawn from the seat's hero, team and
// position, under a new handle.
func (r *roster) newPawn(occupied *seat) *players.Pawn {
	r.nextEntity++
	origin := copyVec3(occupied.position)
	stamina, maxStamina := float32(3), float32(3)
	return &players.Pawn{
		Entity:       r.nextEntity - 1,
		Hero:         occupied.hero,
		Team:         occupied.team,
		Health:       defaultMaxHealth,
		MaxHealth:    defaultMaxHealth,
		Position:     origin,
		EyeAngles:    &geometry.EulerAngles{},
		CameraAngles: &geometry.EulerAngles{},
		Stamina:      &stamina,
		MaxStamina:   &maxStamina,
		Generation:   occupied.conn.GetGeneration(),
		EyePosition:  eyePosition(origin),
		Velocity:     &geometry.Vec3{},
	}
}

// addBot puts a bot in the lowest free slot and returns that slot. The bot is
// ready, has steam ID 0, and Players lists it at once; running mods hear it
// join once the event that added it returns, as a host call never nests an
// event.
func (s *Sandbox) addBot(mod string, options *players.BotOptions) *wasm.Reply {
	// Resolve the hero, then seat the bot.
	hero, err := resolveHero(options.GetHeroName(), options.GetHeroId())
	if err != "" {
		return &wasm.Reply{Error: err}
	}
	slot, ok := s.roster.add(&seat{
		conn:     &players.Connection{Name: options.GetName(), Bot: true, Ready: true},
		addedBy:  mod,
		hero:     hero,
		team:     options.GetTeam(),
		position: copyVec3(options.GetPosition()),
	})
	if !ok {
		return &wasm.Reply{Error: "every player slot is taken"}
	}
	response, _ := (&players.BotResponse{Player: slot}).MarshalVT()
	return &wasm.Reply{Response: response}
}

// removeBot drops a bot this mod added. Like the host, it refuses the human
// and a slot another mod filled.
func (s *Sandbox) removeBot(mod string, slot int32) *wasm.Reply {
	// Free the slot of a bot this mod added.
	occupied := s.roster.seat(slot)
	if occupied == nil || !occupied.conn.GetBot() || occupied.addedBy != mod {
		return &wasm.Reply{Error: "the mod added no bot in that slot"}
	}
	s.roster.remove(slot)
	return &wasm.Reply{}
}

// pawnOf answers Pawn for slot: the stand-in pawn, or none for a spectator
// or an empty slot.
func (s *Sandbox) pawnOf(slot int32) *wasm.Reply {
	response := &players.PawnResponse{}
	if occupied := s.roster.seat(slot); occupied != nil {
		response.Pawn = occupied.pawn.CloneVT()
	}
	data, _ := response.MarshalVT()
	return &wasm.Reply{Response: data}
}

// allPawns answers Pawns with every seat's stand-in pawn, in slot order.
func (s *Sandbox) allPawns() *wasm.Reply {
	response := &players.PawnsResponse{}
	for slot, occupied := range s.roster.occupied() {
		if occupied.pawn != nil {
			response.Pawns = append(response.Pawns, &players.PlayerPawn{
				Player: slot,
				Pawn:   occupied.pawn.CloneVT(),
			})
		}
	}
	data, _ := response.MarshalVT()
	return &wasm.Reply{Response: data}
}

// selectHero gives the human a hero and a new stand-in pawn. A bot keeps the
// hero AddBot gave it, as on the host.
func (s *Sandbox) selectHero(request *players.SelectHeroRequest) *wasm.Reply {
	// Find the human and resolve the hero and team.
	occupied := s.roster.seat(request.GetPlayer())
	if occupied == nil {
		return &wasm.Reply{Error: "no player is in that slot"}
	}
	if occupied.conn.GetBot() {
		return &wasm.Reply{Error: "a bot keeps the hero AddBot gave it"}
	}
	hero, err := resolveHero(request.GetHeroName(), request.GetHeroId())
	if err != "" {
		return &wasm.Reply{Error: err}
	}
	team := request.GetTeam()
	if team != 0 && team != 2 && team != 3 {
		return &wasm.Reply{Error: "team is 2 or 3"}
	}

	// Spawn the hero; team 0 keeps the seat's team.
	if team != 0 {
		occupied.team = team
	}
	occupied.hero = hero
	occupied.pawn = s.roster.newPawn(occupied)
	response, _ := (&players.HeroResponse{Hero: hero}).MarshalVT()
	return &wasm.Reply{Response: response}
}

// changeTeam stores team on the seat and on its pawn, when it has one.
func (s *Sandbox) changeTeam(request *players.ChangeTeamRequest) *wasm.Reply {
	// Find the seat and check the team.
	occupied := s.roster.seat(request.GetPlayer())
	if occupied == nil {
		return &wasm.Reply{Error: "no player is in that slot"}
	}
	team := request.GetTeam()
	if team != 2 && team != 3 {
		return &wasm.Reply{Error: "team is 2 or 3"}
	}
	occupied.team = team
	if occupied.pawn != nil {
		occupied.pawn.Team = team
	}
	return &wasm.Reply{}
}

// spectate drops the seat's stand-in pawn, as a spectator has none.
func (s *Sandbox) spectate(slot int32) *wasm.Reply {
	occupied := s.roster.seat(slot)
	if occupied == nil {
		return &wasm.Reply{Error: "no player is in that slot"}
	}
	occupied.pawn = nil
	return &wasm.Reply{}
}

// killPlayer zeroes the stand-in pawn's health, or does nothing while they
// spectate.
func (s *Sandbox) killPlayer(slot int32) *wasm.Reply {
	occupied := s.roster.seat(slot)
	if occupied == nil {
		return &wasm.Reply{Error: "no player is in that slot"}
	}
	if occupied.pawn != nil {
		occupied.pawn.Health = 0
	}
	return &wasm.Reply{}
}

// respawn gives the seat a new stand-in pawn of its hero, or does nothing
// before the human picks one.
func (s *Sandbox) respawn(slot int32) *wasm.Reply {
	occupied := s.roster.seat(slot)
	if occupied == nil {
		return &wasm.Reply{Error: "no player is in that slot"}
	}
	if occupied.hero != 0 {
		occupied.pawn = s.roster.newPawn(occupied)
	}
	return &wasm.Reply{}
}

// resolveHero returns the hero a request names by its identifier. The game
// resolves names from its own hero data, which the sandbox does not have.
func resolveHero(name string, id uint32) (uint32, string) {
	switch {
	case name != "":
		return 0, "the sandbox resolves heroes by id only"
	case id == 0:
		return 0, "a hero is required"
	}
	return id, ""
}

// answerPlayerCall handles the roster and stand-in pawn host calls. It
// returns nil for a method this file does not answer.
func (s *Sandbox) answerPlayerCall(mod string, call *wasm.Call) *wasm.Reply {
	// Decode the request each method takes.
	method := call.GetMethod()
	var request interface{ UnmarshalVT([]byte) error }
	slot := &players.PlayerRequest{}
	options := &players.BotOptions{}
	selection := &players.SelectHeroRequest{}
	team := &players.ChangeTeamRequest{}
	switch method {
	case "AddBot":
		request = options
	case "SelectHero":
		request = selection
	case "ChangeTeam":
		request = team
	case "RemoveBot", "Pawn", "Spectate", "Kill", "Respawn":
		request = slot
	case "Pawns":
	default:
		return nil
	}
	if request != nil && request.UnmarshalVT(call.GetRequest()) != nil {
		return &wasm.Reply{Error: "the " + method + " request is malformed"}
	}

	// Answer it.
	switch method {
	case "AddBot":
		return s.addBot(mod, options)
	case "RemoveBot":
		return s.removeBot(mod, slot.GetPlayer())
	case "Pawn":
		return s.pawnOf(slot.GetPlayer())
	case "Pawns":
		return s.allPawns()
	case "SelectHero":
		return s.selectHero(selection)
	case "ChangeTeam":
		return s.changeTeam(team)
	case "Spectate":
		return s.spectate(slot.GetPlayer())
	case "Kill":
		return s.killPlayer(slot.GetPlayer())
	default:
		return s.respawn(slot.GetPlayer())
	}
}

// announceTo tells one running instance about a roster it has not seen:
// PlayersChanged with every seat, then, when it takes joins, PlayerLeft for
// each player it heard join who is gone and PlayerJoined for each it has not
// heard. Each instance keeps what it heard, so a reloaded mod hears every
// player again. It runs between events, never inside a host call.
func (s *Sandbox) announceTo(ctx context.Context, run *running) {
	// Skip an instance that stopped or already saw this roster.
	r := s.roster
	if run.mod == nil || run.seen == r.revision {
		return
	}
	run.seen = r.revision
	_ = run.mod.Deliver(ctx, "PlayersChanged", &players.PlayersChangedEvent{Players: r.connections()}, nil)
	if !run.wants.GetJoins() {
		return
	}

	// Report who left before who joined, so a reused slot reads in order.
	for slot, heard := range run.heard {
		occupied := r.seat(int32(slot))
		if heard != nil && (occupied == nil || occupied.conn.GetGeneration() != heard.GetGeneration()) {
			_ = run.mod.Deliver(ctx, "PlayerLeft", &players.PlayerLeftEvent{Connection: heard, Disconnected: true}, nil)
			run.heard[slot] = nil
		}
	}
	for slot, occupied := range r.occupied() {
		if run.heard[slot] == nil {
			run.heard[slot] = occupied.conn.CloneVT()
			_ = run.mod.Deliver(ctx, "PlayerJoined", &players.PlayerJoinedEvent{Connection: occupied.conn.CloneVT()}, nil)
		}
	}
}

// announceAll tells every running mod about a roster it has not seen.
func (s *Sandbox) announceAll(ctx context.Context) {
	for _, run := range s.mods {
		s.announceTo(ctx, run)
	}
}

// copyVec3 copies v, reading a missing vector as the origin.
func copyVec3(v *geometry.Vec3) *geometry.Vec3 {
	if v == nil {
		return &geometry.Vec3{}
	}
	return v.CloneVT()
}

// eyePosition is where a stand-in pawn standing at origin sees from.
func eyePosition(origin *geometry.Vec3) *geometry.Vec3 {
	return &geometry.Vec3{X: origin.GetX(), Y: origin.GetY(), Z: origin.GetZ() + eyeHeight}
}
