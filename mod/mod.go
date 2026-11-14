// Package mod writes Modlock mods in Go. A mod registers its handlers in an
// init function; modlock-host compiles the module to a sandbox inside the game
// server and calls the handlers on the server's frame thread, one at a time.
//
//	package main
//
//	import "github.com/paralin/modlock/mod"
//
//	func init() {
//		mod.Command("hello", func(p mod.Player, args string) {
//			p.Chat("Hello from Go!")
//		})
//	}
//
//	func main() {}
//
// Build it with:
//
//	GOOS=wasip1 GOARCH=wasm go build -buildmode=c-shared -o hello.wasm
//
// The calls into the game and their types are generated from the schema in
// host.gen.go.
package mod

import (
	"errors"
	"maps"
	"slices"
	"strings"

	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/players"
)

// Service answers calls to one service the mod serves: the method's name and
// payload in, the answer out.
type Service func(method string, payload []byte) ([]byte, error)

// handlers holds what the mod registered. A module is exactly one mod, and the
// host enters it through the modlock_event export, which has no receiver, so
// the module keeps one handlers value that init registers into and every event
// reads. Each list runs in registration order.
type handlers struct {
	// commands maps each registered command name to its handler.
	commands map[string]func(Player, string)
	// services maps each service the mod serves to its handler.
	services map[string]Service

	starts        []func(args []string)
	frames        []func(*FrameEvent)
	chats         []func(*ChatEvent)
	joins         []func(*PlayerJoinedEvent)
	leaves        []func(*PlayerLeftEvent)
	worlds        []func(mapName string)
	inputs        []func(p Player, pressed, released uint64)
	restoreds     []func(p Player, err error)
	npcsRestoreds []func(err error)
	damages       []func(*DamageEvent) *DamageResult
	damageds      []func(*DamagedEvent)
	kills         []func(*KillEvent)
	casts         []func(*AbilityCastEvent)
	modifiers     map[string][]func(*ModifierFiredEvent)
	launches      []func(*LaunchEvent)
	impacts       []func(*ImpactEvent)
	landeds       []func(*LandedEvent)
	settings      []func(p Player, key, value string)
	presses       []func(p Player, node string)
	changes       []func(*UiInputEvent)
	rosterWatches []func(players []*Connection)

	// roster is the player list the host last reported, or nil before one.
	roster []*Connection
	// rosters counts the player lists the host reported, so a Cast settles
	// joins and leaves only after a new one.
	rosters uint64
	// generations holds each slot's connection generation in the last player
	// list, and tenures counts the connections each slot has lost since the
	// mod started, so PlayerOf tells a newcomer from the one who left.
	generations map[int32]uint32
	tenures     map[int32]uint32

	// result holds the last encoded Reply until the host has copied it.
	result []byte
}

// registered is the mod's single set of handlers.
var registered = &handlers{
	commands:    map[string]func(Player, string){},
	services:    map[string]Service{},
	generations: map[int32]uint32{},
	tenures:     map[int32]uint32{},
}

// Command calls handler when a player types /name in chat. The handler runs
// with the next server frame, before the frame handlers, and receives the
// text after the name, trimmed of surrounding spaces. Registering a name
// again replaces its handler.
func Command(name string, handler func(p Player, args string)) {
	registered.commands[name] = handler
}

// Serve answers the host's calls to service with handler. Registering a
// service again replaces its handler.
func Serve(service string, handler Service) {
	registered.services[service] = handler
}

// OnStart calls handler once when the server starts the mod, with the
// arguments that follow -- on the modlock-host command line.
func OnStart(handler func(args []string)) {
	registered.starts = append(registered.starts, handler)
}

// OnFrame calls handler once per server frame.
func OnFrame(handler func(f *FrameEvent)) {
	registered.frames = append(registered.frames, handler)
}

// OnChat calls handler with each chat line a player writes, commands
// included, on the next server frame.
func OnChat(handler func(line *ChatEvent)) {
	registered.chats = append(registered.chats, handler)
}

// OnPlayerJoin calls handler once for each player the mod serves when they
// are ready: a human who finished joining, a bot once added, and at start
// each player already in the game.
func OnPlayerJoin(handler func(joined *PlayerJoinedEvent)) {
	registered.joins = append(registered.joins, handler)
}

// OnPlayerLeave calls handler once for each joined player who leaves the mod,
// with the reason the server dropped them when it gave one. A player who
// disconnects still has their slot and hero during the call. A player the mod
// stops serving leaves too, with Disconnected false.
func OnPlayerLeave(handler func(left *PlayerLeftEvent)) {
	registered.leaves = append(registered.leaves, handler)
}

// OnWorld calls handler with the map's name each time a world loads, and at
// start when one already has. Objects and bots from an earlier world are gone
// by then.
func OnWorld(handler func(mapName string)) {
	registered.worlds = append(registered.worlds, handler)
}

// OnInput calls handler with the watched buttons a player pressed and
// released, as Button bits, before the next frame. A button is held from its
// press until its release.
func OnInput(handler func(p Player, pressed, released uint64)) {
	registered.inputs = append(registered.inputs, handler)
}

// OnRestored calls handler when a player's Player.RestoreHero ends: err is
// nil once the target held for a second, or says why it did not.
func OnRestored(handler func(p Player, err error)) {
	registered.restoreds = append(registered.restoreds, handler)
}

// OnNpcsRestored calls handler when RestoreNpcs ends: err is nil once the map
// holds every target, or says why it does not.
func OnNpcsRestored(handler func(err error)) {
	registered.npcsRestoreds = append(registered.npcsRestoreds, handler)
}

// OnDamage calls handler before each hit lands, so it can block the hit or
// change its damage by returning a result; nil lets the hit through. Later
// handlers see the earlier ones' amount.
func OnDamage(handler func(hit *DamageEvent) *DamageResult) {
	registered.damages = append(registered.damages, handler)
}

// OnDamaged calls handler on the frame after each hit lands.
func OnDamaged(handler func(hit *DamagedEvent)) {
	registered.damageds = append(registered.damageds, handler)
}

// OnKill calls handler after each hit that kills a hero or a unit, before the
// next frame. The kill names both entities and the player of each: a slot of
// -1 is a unit's, or a hazard's for the attacker.
func OnKill(handler func(kill *KillEvent)) {
	registered.kills = append(registered.kills, handler)
}

// OnAbilityCast calls handler after each ability the game executes, before
// the next frame: a hero's abilities and the items a player activates, at
// the moment the cast takes effect.
func OnAbilityCast(handler func(cast *AbilityCastEvent)) {
	registered.casts = append(registered.casts, handler)
}

// OnModifierEvent calls handler each time the game fires the modifier event
// with this EModifierEvent name, such as ABILITY_CAST_STARTED, PARRY_SUCCESS
// or PLAYER_RESPAWNED, before the next frame. Register before the mod
// starts. A name the running game lacks is logged once and never fires.
func OnModifierEvent(name string, handler func(event *ModifierFiredEvent)) {
	if registered.modifiers == nil {
		registered.modifiers = map[string][]func(*ModifierFiredEvent){}
	}
	registered.modifiers[name] = append(registered.modifiers[name], handler)
}

// OnLaunch calls handler with each watched projectile's first frame, before
// the next frame. WatchProjectiles names the projectiles.
func OnLaunch(handler func(projectile *LaunchEvent)) {
	registered.launches = append(registered.launches, handler)
}

// OnImpact calls handler when a watched projectile strikes something, inside
// the game's impact: its calls apply before the game continues.
func OnImpact(handler func(impact *ImpactEvent)) {
	registered.impacts = append(registered.impacts, handler)
}

// OnLanded calls handler when a hero lands under the manifest's QuakeWorld
// movement, before the next frame.
func OnLanded(handler func(landing *LandedEvent)) {
	registered.landeds = append(registered.landeds, handler)
}

// OnSettingChanged calls handler when a player's setting changes outside the
// mod, such as on the player's profile. The mod's own Player.SetSetting calls
// do not reach it.
func OnSettingChanged(handler func(p Player, key, value string)) {
	registered.settings = append(registered.settings, handler)
}

// OnUiPress calls handler with the node id of each button a player presses in
// the mod's interface, with the next server frame. Package ui registers it to
// run the buttons' handlers.
func OnUiPress(handler func(p Player, node string)) {
	registered.presses = append(registered.presses, handler)
}

// OnUiInput calls handler with each change a player makes to an input in the
// mod's interface, with the next server frame. Package ui registers it to run
// the inputs' handlers.
func OnUiInput(handler func(event *UiInputEvent)) {
	registered.changes = append(registered.changes, handler)
}

// OnPlayers calls handler with each player list the host reports, before the
// event it arrives with. A slot whose generation changed holds a new
// connection.
func OnPlayers(handler func(players []*Connection)) {
	registered.rosterWatches = append(registered.rosterWatches, handler)
}

// Start runs the start handlers and asks for the events the mod handles.
func (h *handlers) Start(event *StartEvent) (*StartResult, error) {
	for _, handler := range h.starts {
		handler(event.GetArgs())
	}
	// Timers run from frames, so the library always takes them.
	return &StartResult{
		Frames:  true,
		Damage:  len(h.damages) != 0,
		Damaged: len(h.damageds) != 0,
		Chat:    len(h.chats) != 0,
		Joins:   len(h.joins) != 0 || len(h.leaves) != 0,
		Casts:   len(h.casts) != 0,
		Kills:   len(h.kills) != 0,

		ModifierEvents: slices.Sorted(maps.Keys(h.modifiers)),
	}, nil
}

// Frame runs the timers that came due and the frame handlers.
func (h *handlers) Frame(event *FrameEvent) error {
	advanceTimers(event.GetTimeSeconds())
	if len(spawns.handlers) != 0 {
		watchSpawns()
	}
	for _, handler := range h.frames {
		handler(event)
	}
	return nil
}

// Command runs the handler registered for the command's name and claims the
// command when one exists.
func (h *handlers) Command(event *CommandEvent) (*CommandResult, error) {
	// Split the command line into its name and arguments.
	name, args, _ := strings.Cut(strings.TrimSpace(event.GetLine()), " ")
	handler, ok := h.commands[name]
	if !ok {
		return &CommandResult{}, nil
	}

	// Claim the command and run its handler with the next frame. The server
	// reads a client's commands under a CPU budget and drops the client when
	// one handler spends it; slow work in a frame only delays that frame.
	player, args := PlayerOf(event.GetPlayer()), strings.TrimSpace(args)
	After(0, func() { handler(player, args) })
	return &CommandResult{Claimed: true}, nil
}

// Chat runs the chat handlers with the next frame, since the line arrives
// under the same CPU budget as a command.
func (h *handlers) Chat(event *ChatEvent) error {
	After(0, func() {
		for _, handler := range h.chats {
			handler(event)
		}
	})
	return nil
}

// World runs the world handlers.
func (h *handlers) World(event *WorldEvent) error {
	for _, handler := range h.worlds {
		handler(event.GetMap())
	}
	return nil
}

// UiPress runs the press handlers with the next frame, since a press arrives
// under the same CPU budget as a command.
func (h *handlers) UiPress(event *UiPressEvent) error {
	player, node := PlayerOf(event.GetPlayer()), event.GetNode()
	After(0, func() {
		for _, handler := range h.presses {
			handler(player, node)
		}
	})
	return nil
}

// UiInput runs the input handlers with the next frame, as a press does.
func (h *handlers) UiInput(event *UiInputEvent) error {
	After(0, func() {
		for _, handler := range h.changes {
			handler(event)
		}
	})
	return nil
}

// Serve runs the handler of the called service.
func (h *handlers) Serve(event *ServiceCall) (*ServiceReply, error) {
	handler, ok := h.services[event.GetService()]
	if !ok {
		return nil, errors.New("the mod serves no service " + event.GetService())
	}
	payload, err := handler(event.GetMethod(), event.GetPayload())
	if err != nil {
		return nil, err
	}
	return &ServiceReply{Payload: payload}, nil
}

// Damage chains the hit's amount through the damage handlers, stopping at the
// first that blocks it.
func (h *handlers) Damage(event *DamageEvent) (*DamageResult, error) {
	hit := event.CloneVT()
	for _, handler := range h.damages {
		change := handler(hit)
		if change.GetBlock() {
			return change, nil
		}
		if change != nil && change.Amount != nil {
			hit.Amount = change.GetAmount()
		}
	}
	if hit.GetAmount() == event.GetAmount() {
		return &DamageResult{}, nil
	}
	return &DamageResult{Amount: &hit.Amount}, nil
}

// Damaged runs the damaged handlers.
func (h *handlers) Damaged(event *DamagedEvent) error {
	for _, handler := range h.damageds {
		handler(event)
	}
	return nil
}

// Killed runs the kill handlers.
func (h *handlers) Killed(event *KillEvent) error {
	for _, handler := range h.kills {
		handler(event)
	}
	return nil
}

// AbilityCast runs the cast handlers.
func (h *handlers) AbilityCast(event *AbilityCastEvent) error {
	for _, handler := range h.casts {
		handler(event)
	}
	return nil
}

// ModifierFired runs the handlers of the event's name.
func (h *handlers) ModifierFired(event *ModifierFiredEvent) error {
	for _, handler := range h.modifiers[event.GetName()] {
		handler(event)
	}
	return nil
}

// Input runs the input handlers.
func (h *handlers) Input(event *InputEvent) error {
	for _, handler := range h.inputs {
		handler(PlayerOf(event.GetPlayer()), event.GetPressed(), event.GetReleased())
	}
	return nil
}

// Restored runs the restored handlers.
func (h *handlers) Restored(event *RestoredEvent) error {
	err := errorOf(event.GetError())
	for _, handler := range h.restoreds {
		handler(PlayerOf(event.GetPlayer()), err)
	}
	return nil
}

// NpcsRestored runs the NPC restored handlers.
func (h *handlers) NpcsRestored(event *NpcsRestoredEvent) error {
	err := errorOf(event.GetError())
	for _, handler := range h.npcsRestoreds {
		handler(err)
	}
	return nil
}

// Launch runs the launch handlers.
func (h *handlers) Launch(event *LaunchEvent) error {
	for _, handler := range h.launches {
		handler(event)
	}
	return nil
}

// Impact runs the impact handlers.
func (h *handlers) Impact(event *ImpactEvent) error {
	for _, handler := range h.impacts {
		handler(event)
	}
	return nil
}

// Landed runs the landing handlers.
func (h *handlers) Landed(event *LandedEvent) error {
	for _, handler := range h.landeds {
		handler(event)
	}
	return nil
}

// SettingChanged runs the setting handlers.
func (h *handlers) SettingChanged(event *SettingChangedEvent) error {
	for _, handler := range h.settings {
		handler(PlayerOf(event.GetPlayer()), event.GetKey(), event.GetValue())
	}
	return nil
}

// PlayersChanged keeps the reported player list for Players, and ends the
// tenure of each slot whose connection left or was replaced, so PlayerOf
// gives the next holder a different Player. The host reports the list before
// any event once it changed, so every event's Player names its connection.
func (h *handlers) PlayersChanged(event *PlayersChangedEvent) error {
	// Keep the list.
	h.roster = event.GetPlayers()
	if h.roster == nil {
		h.roster = []*Connection{}
	}
	h.rosters++

	// End each tenure whose connection is gone, then note the new list's.
	current := make(map[int32]uint32, len(h.roster))
	for _, connection := range h.roster {
		current[connection.GetPlayer()] = connection.GetGeneration()
	}
	for slot, generation := range h.generations {
		if held, ok := current[slot]; !ok || held != generation {
			h.tenures[slot]++
		}
	}
	h.generations = current

	// Tell the watchers.
	for _, handler := range h.rosterWatches {
		handler(h.roster)
	}
	return nil
}

// PlayerJoined runs the join handlers.
func (h *handlers) PlayerJoined(event *PlayerJoinedEvent) error {
	for _, handler := range h.joins {
		handler(event)
	}
	return nil
}

// PlayerLeft runs the leave handlers at once, so the leaving player's hero
// still reads.
func (h *handlers) PlayerLeft(event *PlayerLeftEvent) error {
	for _, handler := range h.leaves {
		handler(event)
	}
	return nil
}

// roster returns the player list the host last reported, or asks for it.
func roster() []*Connection {
	if held := registered.roster; held != nil {
		return held
	}
	held, _ := Players()
	return held
}

// Player addresses one connected player by their server slot. Two Players
// are equal only when they name the same connection: a person who takes a
// slot another left gets a different Player, so a map keyed by Player never
// hands the newcomer the entry of the one who left. PlayerOf makes one; the
// zero Player names nobody.
type Player struct {
	// slot identifies the player on the server.
	slot int32
	// tenure is one more than the number of connections the slot had lost
	// when the Player was made, so it is zero only in the zero Player.
	tenure uint32
}

// PlayerOf returns the Player of the connection that holds slot now, equal
// to every other Player made for that connection.
func PlayerOf(slot int32) Player {
	return Player{slot: slot, tenure: registered.tenures[slot] + 1}
}

// Slot returns the server slot the player holds.
func (p Player) Slot() int32 {
	return p.slot
}

// Connection returns the player's connection: their name, Steam ID, whether
// they are a bot and whether they finished joining. It returns nil when
// nobody holds the slot.
func (p Player) Connection() *Connection {
	for _, connection := range roster() {
		if connection.GetPlayer() == p.slot {
			return connection
		}
	}
	return nil
}

// ConnectionOfSteamID returns the connection of the account with a Steam ID,
// or nil when it is not connected. Its GetPlayer is the slot of the Player to
// call.
func ConnectionOfSteamID(steamID uint64) *Connection {
	for _, connection := range roster() {
		if connection.GetSteamId() == steamID {
			return connection
		}
	}
	return nil
}

// Players lists the occupied player slots in slot order. It answers from the
// roster the host last reported, and asks the host until a report arrives.
func Players() ([]*Connection, error) {
	if registered.roster != nil {
		return slices.Clone(registered.roster), nil
	}
	response := &players.PlayersResponse{}
	if err := invoke("Players", &wasm.Empty{}, response); err != nil {
		return nil, err
	}
	return response.GetPlayers(), nil
}

// errorOf returns message as an error, or nil when it is empty.
func errorOf(message string) error {
	if message == "" {
		return nil
	}
	return errors.New(message)
}
