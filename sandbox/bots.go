package sandbox

import (
	"context"
	"strconv"

	"github.com/paralin/modlock/src/geometry"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/players"
)

// maxPlayerSlots is the highest slot the sandbox will occupy, human included.
const maxPlayerSlots = 16

// firstBotSlot is the first slot AddBot may give; slot 0 is the stand-in human.
const firstBotSlot = 1

// firstEntity is the first stand-in pawn handle, so a missing pawn stays 0.
const firstEntity = 1000

// defaultMaxHealth is the health a stand-in pawn starts and respawns with.
const defaultMaxHealth int32 = 100

// seat is one occupied slot in the sandbox roster: the stand-in human or a
// bot a mod added, plus the stand-in pawn state those host calls read.
type seat struct {
	// conn is how Players lists this slot.
	conn *players.Connection
	// pawn is the slot's stand-in pawn, or nil while they spectate.
	pawn *players.Pawn
	// addedBy is the slug of the mod that called AddBot; empty for the human.
	addedBy string
	// hero is the last hero SelectHero or AddBot stored, kept across spectate.
	hero uint32
	// team is the last team AddBot, SelectHero or ChangeTeam stored.
	team int32
	// position is where the stand-in pawn stands.
	position *geometry.Vec3
}

// ensureRoster gives the sandbox its stand-in human seat the first time a
// roster call runs, so tests that construct a Sandbox without Start still
// answer Players and AddBot.
func (s *Sandbox) ensureRoster() {
	if s.seats != nil {
		return
	}
	s.seats = map[int32]*seat{0: {
		conn:     player,
		pawn:     s.newPawn(player, 0, 0, &geometry.Vec3{}),
		position: &geometry.Vec3{},
	}}
	s.lastGen = map[int32]uint32{0: player.GetGeneration()}
	s.joined = map[string]map[int32]uint32{}
	s.pendingLeft = map[string][]*players.Connection{}
	s.nextEntity = firstEntity
}

// connections lists every occupied seat, human first, then slots in order.
func (s *Sandbox) connections() []*players.Connection {
	s.ensureRoster()
	list := make([]*players.Connection, 0, len(s.seats))
	for slot := int32(0); slot < maxPlayerSlots; slot++ {
		if occupied := s.seats[slot]; occupied != nil {
			list = append(list, cloneConnection(occupied.conn))
		}
	}
	return list
}

// addBot puts a bot in the next free slot and returns it. The bot is ready,
// has steam ID 0, and is listed by Players immediately. Join events wait
// until the current host call's event finishes, so they are not nested.
func (s *Sandbox) addBot(mod string, options *players.BotOptions) *wasm.Reply {
	s.ensureRoster()
	if options == nil {
		return &wasm.Reply{Error: "the AddBot request is malformed"}
	}
	hero, err := s.resolveHero(options.GetHeroName(), options.GetHeroId(), options.GetHero() != nil, true)
	if err != "" {
		return &wasm.Reply{Error: err}
	}
	slot, ok := s.freeSlot()
	if !ok {
		return &wasm.Reply{Error: "every player slot is taken"}
	}
	name := options.GetName()
	if name == "" {
		name = options.GetHeroName()
	}
	if name == "" {
		name = "Bot " + strconv.Itoa(int(slot))
	}
	generation := s.lastGen[slot] + 1
	if generation == 0 {
		generation = 1
	}
	s.lastGen[slot] = generation
	conn := &players.Connection{
		Player:     slot,
		SteamId:    0,
		Name:       name,
		Bot:        true,
		Ready:      true,
		Generation: generation,
	}
	position := copyVec3(options.GetPosition())
	team := options.GetTeam()
	s.seats[slot] = &seat{
		conn:     conn,
		pawn:     s.newPawn(conn, hero, team, position),
		addedBy:  mod,
		hero:     hero,
		team:     team,
		position: position,
	}
	s.rosterDirty = true
	response, _ := (&players.BotResponse{Player: slot}).MarshalVT()
	return &wasm.Reply{Response: response}
}

// removeBot drops a bot this mod added. It refuses the stand-in human and a
// slot another mod filled, as the host reports when the mod added no bot
// there.
func (s *Sandbox) removeBot(mod string, slot int32) *wasm.Reply {
	s.ensureRoster()
	occupied, ok := s.seats[slot]
	if !ok || !occupied.conn.GetBot() || occupied.addedBy != mod {
		return &wasm.Reply{Error: "the mod added no bot in that slot"}
	}
	s.leaveSeat(slot)
	return &wasm.Reply{}
}

// pawnOf answers Pawn for slot: the stand-in pawn, or empty for a spectator
// or an unknown slot.
func (s *Sandbox) pawnOf(slot int32) *wasm.Reply {
	s.ensureRoster()
	response := &players.PawnResponse{}
	if occupied, ok := s.seats[slot]; ok && occupied.pawn != nil {
		response.Pawn = clonePawn(occupied.pawn)
	}
	data, _ := response.MarshalVT()
	return &wasm.Reply{Response: data}
}

// allPawns answers Pawns with every occupied seat that has a stand-in pawn.
func (s *Sandbox) allPawns() *wasm.Reply {
	s.ensureRoster()
	response := &players.PawnsResponse{}
	for slot := int32(0); slot < maxPlayerSlots; slot++ {
		occupied := s.seats[slot]
		if occupied == nil || occupied.pawn == nil {
			continue
		}
		response.Pawns = append(response.Pawns, &players.PlayerPawn{
			Player: slot,
			Pawn:   clonePawn(occupied.pawn),
		})
	}
	data, _ := response.MarshalVT()
	return &wasm.Reply{Response: data}
}

// selectHero gives the human a hero and a stand-in pawn. A bot keeps the
// hero AddBot gave it.
func (s *Sandbox) selectHero(request *players.SelectHeroRequest) *wasm.Reply {
	s.ensureRoster()
	if request == nil {
		return &wasm.Reply{Error: "the SelectHero request is malformed"}
	}
	occupied, ok := s.seats[request.GetPlayer()]
	if !ok {
		return &wasm.Reply{Error: "no such player"}
	}
	if occupied.conn.GetBot() {
		return &wasm.Reply{Error: "a bot keeps the hero AddBot gave it"}
	}
	hero, err := s.resolveHero(request.GetHeroName(), request.GetHeroId(), request.GetHero() != nil, false)
	if err != "" {
		return &wasm.Reply{Error: err}
	}
	team := request.GetTeam()
	if team != 0 {
		if team != 2 && team != 3 {
			return &wasm.Reply{Error: "team is 2 or 3"}
		}
		occupied.team = team
	}
	occupied.hero = hero
	occupied.pawn = s.newPawn(occupied.conn, hero, occupied.team, occupied.position)
	response, _ := (&players.HeroResponse{Hero: hero}).MarshalVT()
	return &wasm.Reply{Response: response}
}

// changeTeam stores team on the seat and its pawn, when it has one.
func (s *Sandbox) changeTeam(request *players.ChangeTeamRequest) *wasm.Reply {
	s.ensureRoster()
	if request == nil {
		return &wasm.Reply{Error: "the ChangeTeam request is malformed"}
	}
	occupied, ok := s.seats[request.GetPlayer()]
	if !ok {
		return &wasm.Reply{Error: "no such player"}
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
	s.ensureRoster()
	occupied, ok := s.seats[slot]
	if !ok {
		return &wasm.Reply{Error: "no such player"}
	}
	occupied.pawn = nil
	return &wasm.Reply{}
}

// killPlayer zeroes the stand-in pawn's health, or does nothing while they
// spectate.
func (s *Sandbox) killPlayer(slot int32) *wasm.Reply {
	s.ensureRoster()
	occupied, ok := s.seats[slot]
	if !ok {
		return &wasm.Reply{Error: "no such player"}
	}
	if occupied.pawn != nil {
		occupied.pawn.Health = 0
	}
	return &wasm.Reply{}
}

// respawn restores a dead stand-in pawn with its stored hero, or does
// nothing if the seat has no hero.
func (s *Sandbox) respawn(slot int32) *wasm.Reply {
	s.ensureRoster()
	occupied, ok := s.seats[slot]
	if !ok {
		return &wasm.Reply{Error: "no such player"}
	}
	if occupied.hero == 0 && (occupied.pawn == nil || occupied.pawn.GetHero() == 0) {
		return &wasm.Reply{}
	}
	hero := occupied.hero
	if hero == 0 {
		hero = occupied.pawn.GetHero()
	}
	occupied.hero = hero
	occupied.pawn = s.newPawn(occupied.conn, hero, occupied.team, occupied.position)
	return &wasm.Reply{}
}

// answerPlayerCall handles the roster and stand-in pawn host calls. It
// returns nil for a method this file does not answer.
func (s *Sandbox) answerPlayerCall(mod string, call *wasm.Call) *wasm.Reply {
	switch call.GetMethod() {
	case "AddBot":
		options := &players.BotOptions{}
		if err := options.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the AddBot request is malformed"}
		}
		return s.addBot(mod, options)
	case "RemoveBot":
		request := &players.PlayerRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the RemoveBot request is malformed"}
		}
		return s.removeBot(mod, request.GetPlayer())
	case "Pawn":
		request := &players.PlayerRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the Pawn request is malformed"}
		}
		return s.pawnOf(request.GetPlayer())
	case "Pawns":
		return s.allPawns()
	case "SelectHero":
		request := &players.SelectHeroRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the SelectHero request is malformed"}
		}
		return s.selectHero(request)
	case "ChangeTeam":
		request := &players.ChangeTeamRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the ChangeTeam request is malformed"}
		}
		return s.changeTeam(request)
	case "Spectate":
		request := &players.PlayerRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the Spectate request is malformed"}
		}
		return s.spectate(request.GetPlayer())
	case "Kill":
		request := &players.PlayerRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the Kill request is malformed"}
		}
		return s.killPlayer(request.GetPlayer())
	case "Respawn":
		request := &players.PlayerRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the Respawn request is malformed"}
		}
		return s.respawn(request.GetPlayer())
	default:
		return nil
	}
}

// takeModBots removes the bots mod added and returns them so a failed reload
// can put them back. Other running mods are queued a PlayerLeft for each.
func (s *Sandbox) takeModBots(mod string) []*seat {
	s.ensureRoster()
	var taken []*seat
	for slot := int32(firstBotSlot); slot < maxPlayerSlots; slot++ {
		occupied := s.seats[slot]
		if occupied == nil || occupied.addedBy != mod {
			continue
		}
		taken = append(taken, occupied)
		s.queueLeft(mod, occupied.conn)
		delete(s.seats, slot)
		s.rosterDirty = true
	}
	return taken
}

// restoreBots puts seats takeModBots removed back, and drops the PlayerLeft
// events queued for them, for a reload whose new instance failed to start.
func (s *Sandbox) restoreBots(taken []*seat) {
	if len(taken) == 0 {
		return
	}
	s.ensureRoster()
	slots := map[int32]bool{}
	for _, occupied := range taken {
		s.seats[occupied.conn.GetPlayer()] = occupied
		slots[occupied.conn.GetPlayer()] = true
	}
	for name, left := range s.pendingLeft {
		kept := left[:0]
		for _, conn := range left {
			if !slots[conn.GetPlayer()] {
				kept = append(kept, conn)
			}
		}
		s.pendingLeft[name] = kept
	}
}

// dropModBots removes the bots mod added, as when the world ends or the mod
// stops, and queues PlayerLeft for the other running mods.
func (s *Sandbox) dropModBots(mod string) {
	s.takeModBots(mod)
}

// leaveSeat removes slot from the roster and queues PlayerLeft for running
// mods that already heard that bot join.
func (s *Sandbox) leaveSeat(slot int32) {
	occupied, ok := s.seats[slot]
	if !ok {
		return
	}
	s.queueLeft("", occupied.conn)
	delete(s.seats, slot)
	s.rosterDirty = true
}

// queueLeft records a PlayerLeft for every running mod except skip, which is
// the mod being replaced and will not hear its own bots leave.
func (s *Sandbox) queueLeft(skip string, conn *players.Connection) {
	for _, run := range s.mods {
		if run == nil || run.name == skip || run.mod == nil {
			continue
		}
		s.pendingLeft[run.name] = append(s.pendingLeft[run.name], cloneConnection(conn))
	}
}

// announceTo tells one running mod the current roster, then any joins or
// leaves it has not heard yet. It is not used from inside a host call.
func (s *Sandbox) announceTo(ctx context.Context, run *running) {
	if run == nil || run.mod == nil {
		return
	}
	s.ensureRoster()
	_ = run.mod.Deliver(ctx, "PlayersChanged", &players.PlayersChangedEvent{Players: s.connections()}, nil)
	if s.joined[run.name] == nil {
		s.joined[run.name] = map[int32]uint32{}
	}
	announced := s.joined[run.name]
	if run.wants != nil && run.wants.GetJoins() {
		for _, left := range s.pendingLeft[run.name] {
			_ = run.mod.Deliver(ctx, "PlayerLeft", &players.PlayerLeftEvent{
				Connection:   left,
				Disconnected: true,
			}, nil)
			delete(announced, left.GetPlayer())
		}
		for slot := int32(0); slot < maxPlayerSlots; slot++ {
			occupied := s.seats[slot]
			if occupied == nil {
				continue
			}
			generation := occupied.conn.GetGeneration()
			if announced[slot] == generation {
				continue
			}
			_ = run.mod.Deliver(ctx, "PlayerJoined", &players.PlayerJoinedEvent{Connection: cloneConnection(occupied.conn)}, nil)
			announced[slot] = generation
		}
	} else {
		for _, left := range s.pendingLeft[run.name] {
			delete(announced, left.GetPlayer())
		}
	}
	s.pendingLeft[run.name] = nil
}

// announceAll reports a dirty roster to every running mod, once the event
// that added or removed a bot has finished.
func (s *Sandbox) announceAll(ctx context.Context) {
	if !s.rosterDirty && !s.hasPendingLeft() {
		return
	}
	for _, run := range s.mods {
		s.announceTo(ctx, run)
	}
	s.rosterDirty = false
}

// announceOthers reports a dirty roster to every running mod except skip,
// used after a reload so the new instance is not told twice.
func (s *Sandbox) announceOthers(ctx context.Context, skip string) {
	if !s.rosterDirty && !s.hasPendingLeft() {
		return
	}
	for _, run := range s.mods {
		if run != nil && run.name == skip {
			continue
		}
		s.announceTo(ctx, run)
	}
	s.rosterDirty = false
}

func (s *Sandbox) hasPendingLeft() bool {
	for _, left := range s.pendingLeft {
		if len(left) > 0 {
			return true
		}
	}
	return false
}

// freeSlot returns the lowest empty slot a bot may take.
func (s *Sandbox) freeSlot() (int32, bool) {
	for slot := int32(firstBotSlot); slot < maxPlayerSlots; slot++ {
		if s.seats[slot] == nil {
			return slot, true
		}
	}
	return 0, false
}

// resolveHero accepts a hero identifier, and refuses a name the sandbox
// cannot look up without the game.
func (s *Sandbox) resolveHero(name string, id uint32, set, bot bool) (uint32, string) {
	if name != "" {
		return 0, "the game has no such hero"
	}
	if !set || id == 0 {
		if bot {
			return 0, "a bot needs a hero"
		}
		return 0, "a hero is required"
	}
	return id, ""
}

// newPawn builds a stand-in pawn from the seat's stored hero, team and place.
func (s *Sandbox) newPawn(conn *players.Connection, hero uint32, team int32, position *geometry.Vec3) *players.Pawn {
	if s.nextEntity < firstEntity {
		s.nextEntity = firstEntity
	}
	s.nextEntity++
	origin := copyVec3(position)
	stamina := float32(3)
	maxStamina := float32(3)
	return &players.Pawn{
		Entity:       s.nextEntity - 1,
		Hero:         hero,
		Team:         team,
		Health:       defaultMaxHealth,
		MaxHealth:    defaultMaxHealth,
		Position:     origin,
		EyeAngles:    &geometry.EulerAngles{},
		CameraAngles: &geometry.EulerAngles{},
		Stamina:      &stamina,
		MaxStamina:   &maxStamina,
		Generation:   conn.GetGeneration(),
		EyePosition: &geometry.Vec3{
			X: origin.X,
			Y: origin.Y,
			Z: origin.Z + 64,
		},
		Velocity: &geometry.Vec3{},
	}
}

func copyVec3(v *geometry.Vec3) *geometry.Vec3 {
	if v == nil {
		return &geometry.Vec3{}
	}
	return &geometry.Vec3{X: v.X, Y: v.Y, Z: v.Z}
}

func cloneConnection(conn *players.Connection) *players.Connection {
	if conn == nil {
		return nil
	}
	copied := *conn
	if conn.Roles != nil {
		copied.Roles = append([]players.ModRole{}, conn.Roles...)
	}
	return &copied
}

func clonePawn(pawn *players.Pawn) *players.Pawn {
	if pawn == nil {
		return nil
	}
	copied := *pawn
	copied.Position = copyVec3(pawn.Position)
	if pawn.EyeAngles != nil {
		angles := *pawn.EyeAngles
		copied.EyeAngles = &angles
	}
	if pawn.CameraAngles != nil {
		angles := *pawn.CameraAngles
		copied.CameraAngles = &angles
	}
	copied.EyePosition = copyVec3(pawn.EyePosition)
	copied.Velocity = copyVec3(pawn.Velocity)
	if pawn.CameraPosition != nil {
		copied.CameraPosition = copyVec3(pawn.CameraPosition)
	}
	if pawn.Stamina != nil {
		value := *pawn.Stamina
		copied.Stamina = &value
	}
	if pawn.MaxStamina != nil {
		value := *pawn.MaxStamina
		copied.MaxStamina = &value
	}
	if pawn.CarryingUrn != nil {
		value := *pawn.CarryingUrn
		copied.CarryingUrn = &value
	}
	if pawn.EffectiveMaxHealth != nil {
		value := *pawn.EffectiveMaxHealth
		copied.EffectiveMaxHealth = &value
	}
	return &copied
}
