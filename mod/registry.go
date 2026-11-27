package mod

import (
	"errors"
	"maps"
	"slices"
	"strings"
)

// Registry owns ordered callbacks and named command and service handlers.
// Its zero value accepts registrations. Host entries and registrations run on
// the same serial frame thread; a Registry does not synchronize callers.
// Registration during delivery preserves Go range semantics: each range uses
// its initial length. Deferred chat and UI delivery reads the list at execution.
type Registry struct {
	// spawns holds hero-spawn callbacks in registration order.
	spawns []func(Player, *Pawn)
	// commands maps each registered command name to its handler.
	commands map[string]func(Player, string)
	// services maps each service the mod serves to its handler.
	services map[string]Service

	// starts holds its callbacks in registration order.
	starts []func(args []string)
	// frames holds its callbacks in registration order.
	frames []func(*FrameEvent)
	// chats holds its callbacks in registration order.
	chats []func(*ChatEvent)
	// joins holds its callbacks in registration order.
	joins []func(*PlayerJoinedEvent)
	// leaves holds its callbacks in registration order.
	leaves []func(*PlayerLeftEvent)
	// aways holds its callbacks in registration order.
	aways []func(*PlayerAwayEvent)
	// returns holds its callbacks in registration order.
	returns []func(*PlayerReturnedEvent)
	// worlds holds its callbacks in registration order.
	worlds []func(mapName string)
	// inputs holds its callbacks in registration order.
	inputs []func(p Player, pressed, released uint64)
	// restoreds holds its callbacks in registration order.
	restoreds []func(p Player, err error)
	// npcsRestoreds holds its callbacks in registration order.
	npcsRestoreds []func(err error)
	// damages holds its callbacks in registration order.
	damages []func(*DamageEvent) *DamageResult
	// pings holds its callbacks in registration order.
	pings []func(*PingEvent) *PingResult
	// damageds holds its callbacks in registration order.
	damageds []func(*DamagedEvent)
	// kills holds its callbacks in registration order.
	kills []func(*KillEvent)
	// casts holds its callbacks in registration order.
	casts []func(*AbilityCastEvent)
	// heals holds its callbacks in registration order.
	heals []func(*HealEvent)
	// levels holds its callbacks in registration order.
	levels []func(*LevelEvent)
	// melees holds its callbacks in registration order.
	melees []func(*MeleeEvent)
	// parries holds its callbacks in registration order.
	parries []func(*ParryEvent)
	// modifiers holds its callbacks in registration order.
	modifiers map[string][]func(*ModifierFiredEvent)
	// launches holds its callbacks in registration order.
	launches []func(*LaunchEvent)
	// impacts holds its callbacks in registration order.
	impacts []func(*ImpactEvent)
	// landeds holds its callbacks in registration order.
	landeds []func(*LandedEvent)
	// settings holds its callbacks in registration order.
	settings []func(p Player, key, value string)
	// presses holds its callbacks in registration order.
	presses []func(event *UiPressEvent)
	// changes holds its callbacks in registration order.
	changes []func(*UiInputEvent)
	// documents holds its callbacks in registration order.
	documents []func(*UiDocumentEvent)
	// actions holds its callbacks in registration order.
	actions []func(*UiActionEvent)
	// overlays holds its callbacks in registration order.
	overlays []func(*UiOverlayEvent)
	// placements holds its callbacks in registration order.
	placements []func(*UiPlacementEvent)
	// windows holds its callbacks in registration order.
	windows []func(*UiWindowEvent)
	// rosterWatches holds its callbacks in registration order.
	rosterWatches []func(players []*Connection)
}

// RegisterCommand calls handler when a player types /name in chat. The handler runs
// with the next server frame, before the frame handlers, and receives the
// text after the name, trimmed of surrounding spaces. Registering a name
// again replaces its handler.
func (r *Registry) RegisterCommand(name string, handler func(p Player, args string)) {
	if r.commands == nil {
		r.commands = map[string]func(Player, string){}
	}
	r.commands[name] = handler
}

// RegisterService answers the host's calls to service with handler. Registering a
// service again replaces its handler.
func (r *Registry) RegisterService(service string, handler Service) {
	if r.services == nil {
		r.services = map[string]Service{}
	}
	r.services[service] = handler
}

// OnSpawn calls handler when the adapter reports a living hero entering play.
func (r *Registry) OnSpawn(handler func(Player, *Pawn)) {
	r.spawns = append(r.spawns, handler)
}

// Spawn delivers a spawn accepted by the adapter's duplicate filter.
func (r *Registry) Spawn(player Player, pawn *Pawn) {
	for _, handler := range r.spawns {
		handler(player, pawn)
	}
}

// OnStart calls handler once when the server starts the mod, with the
// words after -- on the modlock dev or play command line.
func (r *Registry) OnStart(handler func(args []string)) {
	r.starts = append(r.starts, handler)
}

// OnFrame calls handler once per server frame.
func (r *Registry) OnFrame(handler func(f *FrameEvent)) {
	r.frames = append(r.frames, handler)
}

// OnChat calls handler with each chat line a player writes, commands
// included, on the next server frame.
func (r *Registry) OnChat(handler func(line *ChatEvent)) {
	r.chats = append(r.chats, handler)
}

// OnPlayerJoin calls handler once for each player the mod serves when they
// are ready: a human who finished joining, a bot once added, and at start
// each player already in the game.
func (r *Registry) OnPlayerJoin(handler func(joined *PlayerJoinedEvent)) {
	r.joins = append(r.joins, handler)
}

// OnPlayerAway calls handler once each time a joined human's client
// disconnects while the server holds their seat. They have not left: their
// hero and controller stay in the game, and OnPlayerReturn follows if they
// come back, so keep their seat as it stands rather than clear it.
func (r *Registry) OnPlayerAway(handler func(away *PlayerAwayEvent)) {
	r.aways = append(r.aways, handler)
}

// OnPlayerReturn calls handler once each time an away player reconnects to
// their seat and finishes joining again, with the same Player, generation and
// hero they left.
func (r *Registry) OnPlayerReturn(handler func(returned *PlayerReturnedEvent)) {
	r.returns = append(r.returns, handler)
}

// OnPlayerLeave calls handler once for each joined player who leaves the mod,
// with the reason the server dropped them when it gave one. A seat ends, and
// its player leaves, when a bot is removed, a human is kicked or released by
// the program that runs the host, or a human stays away past the game's
// limit; a human who only disconnects is away first. The slot and hero still
// read during the call, and afterwards Modlock kills the hero, keeps it down
// and takes the controller off its team, so the mod need not clear them. A
// player the mod stops serving leaves too, with Disconnected false.
func (r *Registry) OnPlayerLeave(handler func(left *PlayerLeftEvent)) {
	r.leaves = append(r.leaves, handler)
}

// OnWorld calls handler with the map's name each time a world loads, and at
// start when one already has. Objects and bots from an earlier world are gone
// by then.
func (r *Registry) OnWorld(handler func(mapName string)) {
	r.worlds = append(r.worlds, handler)
}

// OnInput calls handler with the watched buttons a player pressed and
// released, as Button bits, before the next frame. A button is held from its
// press until its release.
func (r *Registry) OnInput(handler func(p Player, pressed, released uint64)) {
	r.inputs = append(r.inputs, handler)
}

// OnRestored calls handler when a player's Player.RestoreHero ends: err is
// nil once the target held for a second, or says why it did not.
func (r *Registry) OnRestored(handler func(p Player, err error)) {
	r.restoreds = append(r.restoreds, handler)
}

// OnNpcsRestored calls handler when RestoreNpcs ends: err is nil once the map
// holds every target, or says why it does not.
func (r *Registry) OnNpcsRestored(handler func(err error)) {
	r.npcsRestoreds = append(r.npcsRestoreds, handler)
}

// OnDamage calls handler before each hit lands, so it can block the hit or
// change its damage by returning a result; nil lets the hit through. Later
// handlers see the earlier ones' amount.
func (r *Registry) OnDamage(handler func(hit *DamageEvent) *DamageResult) {
	r.damages = append(r.damages, handler)
}

// OnPing calls handler with each ping a player sends, from the comms wheel, a
// quick ping key, the map or an ability, before anyone sees it, so it can
// block the ping or mute its voice line. A blocked ping reaches nobody, the
// sender included: no marker, sound, voice line or chat line. The players a
// handler lists in MuteVoice, by slot, get the ping without its voice line.
// The handler runs inside the server's ping, so it decides at once and defers
// other work to a frame.
func (r *Registry) OnPing(handler func(ping *PingEvent) *PingResult) {
	r.pings = append(r.pings, handler)
}

// OnDamaged calls handler on the frame after each hit lands.
func (r *Registry) OnDamaged(handler func(hit *DamagedEvent)) {
	r.damageds = append(r.damageds, handler)
}

// OnKill calls handler after each hit that kills a hero or a unit, before the
// next frame. The kill names both entities and the player of each: a slot of
// -1 is a unit's, or a hazard's for the attacker.
func (r *Registry) OnKill(handler func(kill *KillEvent)) {
	r.kills = append(r.kills, handler)
}

// OnAbilityCast calls handler after each ability the game executes, before
// the next frame: a hero's abilities and the items a player activates, at
// the moment the cast takes effect.
func (r *Registry) OnAbilityCast(handler func(cast *AbilityCastEvent)) {
	r.casts = append(r.casts, handler)
}

// OnHeal calls handler after each heal, before the next frame: health a hero
// or unit regained at once from an ability, an item or lifesteal. Regeneration
// calls nothing, including what abilities such as Siphon Life restore as
// regeneration, and neither do the mod's own heal and a heal at full health.
// The heal names the entity healed, the healer and ability when the game
// names them, and the player of each: a slot of -1 is a unit's, or no one's.
func (r *Registry) OnHeal(handler func(heal *HealEvent)) {
	r.heals = append(r.heals, handler)
}

// OnLevelChange calls handler as each hero reaches a new level, before the
// next frame.
func (r *Registry) OnLevelChange(handler func(change *LevelEvent)) {
	r.levels = append(r.levels, handler)
}

// OnMelee calls handler after each melee swing, before the next frame, with
// whether it was heavy and the entities it struck, none for a miss.
func (r *Registry) OnMelee(handler func(swing *MeleeEvent)) {
	r.melees = append(r.melees, handler)
}

// OnParry calls handler after each parry that catches a melee swing, before
// the next frame, naming the hero that parried and the attacker it caught.
func (r *Registry) OnParry(handler func(parry *ParryEvent)) {
	r.parries = append(r.parries, handler)
}

// OnModifierEvent calls handler each time the game fires the modifier event
// with this EModifierEvent name, such as ABILITY_CAST_STARTED, PARRY_SUCCESS
// or PLAYER_RESPAWNED, before the next frame. Register before the mod
// starts. A name the running game lacks is logged once and never fires.
func (r *Registry) OnModifierEvent(name string, handler func(event *ModifierFiredEvent)) {
	if r.modifiers == nil {
		r.modifiers = map[string][]func(*ModifierFiredEvent){}
	}
	r.modifiers[name] = append(r.modifiers[name], handler)
}

// OnLaunch calls handler with each watched projectile's first frame, before
// the next frame. WatchProjectiles names the projectiles.
func (r *Registry) OnLaunch(handler func(projectile *LaunchEvent)) {
	r.launches = append(r.launches, handler)
}

// OnImpact calls handler when a watched projectile strikes something, inside
// the game's impact: its calls apply before the game continues.
func (r *Registry) OnImpact(handler func(impact *ImpactEvent)) {
	r.impacts = append(r.impacts, handler)
}

// OnLanded calls handler when a hero lands under the manifest's QuakeWorld
// movement, before the next frame.
func (r *Registry) OnLanded(handler func(landing *LandedEvent)) {
	r.landeds = append(r.landeds, handler)
}

// OnSettingChanged calls handler when a player's setting changes outside the
// mod, such as on the player's profile. The mod's own Player.SetSetting calls
// do not reach it.
func (r *Registry) OnSettingChanged(handler func(p Player, key, value string)) {
	r.settings = append(r.settings, handler)
}

// OnUiPress calls handler with each press a player makes in the mod's
// interface, with the next server frame: a button's, or a layout's with the
// action its bound panel or its script names. Package ui registers it to run
// the buttons' and the bindings' handlers.
func (r *Registry) OnUiPress(handler func(event *UiPressEvent)) {
	r.presses = append(r.presses, handler)
}

// OnUiInput calls handler with each change a player makes to an input in the
// mod's interface, with the next server frame. Package ui registers it to run
// the inputs' handlers.
func (r *Registry) OnUiInput(handler func(event *UiInputEvent)) {
	r.changes = append(r.changes, handler)
}

// OnUiDocument calls handler with the end of each submission from a player's
// document input, with the next server frame: the whole document, or why it
// did not arrive.
func (r *Registry) OnUiDocument(handler func(event *UiDocumentEvent)) {
	r.documents = append(r.documents, handler)
}

// OnUiAction calls handler with each phase of a game action the root of a
// player's interface follows, with the next server frame. Package ui
// registers it to run the followers' handlers.
func (r *Registry) OnUiAction(handler func(event *UiActionEvent)) {
	r.actions = append(r.actions, handler)
}

// OnUiOverlay calls handler with each stock overlay that takes or gives back
// the input of a player whose interface root follows overlays, with the next
// server frame. Package ui registers it to run the followers' handlers.
func (r *Registry) OnUiOverlay(handler func(event *UiOverlayEvent)) {
	r.overlays = append(r.overlays, handler)
}

// OnUiPlacement calls handler with each placement of the interface of a
// player whose interface root follows placement, with the next server frame.
// Package ui registers it to run the followers' handlers.
func (r *Registry) OnUiPlacement(handler func(event *UiPlacementEvent)) {
	r.placements = append(r.placements, handler)
}

// OnUiWindow calls handler with each run of a list's rows a player's screen
// asks the mod to show, with the next server frame. Package ui registers it
// to draw the window it asks for.
func (r *Registry) OnUiWindow(handler func(event *UiWindowEvent)) {
	r.windows = append(r.windows, handler)
}

// OnPlayers calls handler with each player list the host reports, before the
// event it arrives with. A slot whose generation changed holds a new
// connection.
func (r *Registry) OnPlayers(handler func(players []*Connection)) {
	r.rosterWatches = append(r.rosterWatches, handler)
}

// Start runs the start handlers and asks for the events the mod handles.
func (h *Registry) Start(event *StartEvent) (*StartResult, error) {
	for _, handler := range h.starts {
		handler(event.GetArgs())
	}
	// Timers run from frames, so the library always takes them.
	return &StartResult{
		Frames:  true,
		Damage:  len(h.damages) != 0,
		Damaged: len(h.damageds) != 0,
		Chat:    len(h.chats) != 0,
		Joins:   len(h.joins) != 0 || len(h.leaves) != 0 || len(h.aways) != 0 || len(h.returns) != 0,
		Casts:   len(h.casts) != 0,
		Kills:   len(h.kills) != 0,
		Heals:   len(h.heals) != 0,
		Levels:  len(h.levels) != 0,
		Melees:  len(h.melees) != 0,
		Parries: len(h.parries) != 0,
		Pings:   len(h.pings) != 0,

		ModifierEvents: h.modifierEvents(),
	}, nil
}

// modifierEvents names the modifier events the mod's handlers watch.
func (h *Registry) modifierEvents() []string {
	names := slices.Collect(maps.Keys(h.modifiers))
	if _, ok := h.modifiers[respawnedEvent]; !ok && h.watchesSpawns() {
		names = append(names, respawnedEvent)
	}
	slices.Sort(names)
	return names
}

// Frame runs frame handlers in registration order.
func (h *Registry) Frame(event *FrameEvent) error {
	for _, handler := range h.frames {
		handler(event)
	}
	return nil
}

// Command runs the handler registered for the command's name and claims the
// command when one exists.
func (h *Registry) Command(event *CommandEvent) (*CommandResult, error) {
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
func (h *Registry) Chat(event *ChatEvent) error {
	After(0, func() {
		for _, handler := range h.chats {
			handler(event)
		}
	})
	return nil
}

// World runs the world handlers.
func (h *Registry) World(event *WorldEvent) error {
	for _, handler := range h.worlds {
		handler(event.GetMap())
	}
	return nil
}

// UiPress runs the press handlers with the next frame, since a press arrives
// under the same CPU budget as a command.
func (h *Registry) UiPress(event *UiPressEvent) error {
	After(0, func() {
		for _, handler := range h.presses {
			handler(event)
		}
	})
	return nil
}

// UiInput runs the input handlers with the next frame, as a press does.
func (h *Registry) UiInput(event *UiInputEvent) error {
	After(0, func() {
		for _, handler := range h.changes {
			handler(event)
		}
	})
	return nil
}

// UiDocument runs the document handlers with the next frame, as a change does.
func (h *Registry) UiDocument(event *UiDocumentEvent) error {
	After(0, func() {
		for _, handler := range h.documents {
			handler(event)
		}
	})
	return nil
}

// UiAction runs the game action handlers with the next frame, as a press does.
func (h *Registry) UiAction(event *UiActionEvent) error {
	After(0, func() {
		for _, handler := range h.actions {
			handler(event)
		}
	})
	return nil
}

// UiOverlay runs the overlay handlers with the next frame, as a press does.
func (h *Registry) UiOverlay(event *UiOverlayEvent) error {
	After(0, func() {
		for _, handler := range h.overlays {
			handler(event)
		}
	})
	return nil
}

// UiPlacement runs the placement handlers with the next frame, as a press
// does.
func (h *Registry) UiPlacement(event *UiPlacementEvent) error {
	After(0, func() {
		for _, handler := range h.placements {
			handler(event)
		}
	})
	return nil
}

// UiWindow runs the window handlers with the next frame, as a press does.
func (h *Registry) UiWindow(event *UiWindowEvent) error {
	After(0, func() {
		for _, handler := range h.windows {
			handler(event)
		}
	})
	return nil
}

// Serve runs the handler of the called service.
func (h *Registry) Serve(event *ServiceCall) (*ServiceReply, error) {
	// Find the most recently registered provider.
	handler, ok := h.services[event.GetService()]
	if !ok {
		return nil, errors.New("the mod serves no service " + event.GetService())
	}

	// Return its payload or preserve the provider's error.
	payload, err := handler(event.GetMethod(), event.GetPayload())
	if err != nil {
		return nil, err
	}
	return &ServiceReply{Payload: payload}, nil
}

// Damage chains the hit's amount through the damage handlers, stopping at the
// first that blocks it.
func (h *Registry) Damage(event *DamageEvent) (*DamageResult, error) {
	// Carry changes in a copy so the incoming event remains unchanged.
	hit := event.CloneVT()

	// Offer the current amount to each handler until one blocks the hit.
	for _, handler := range h.damages {
		change := handler(hit)
		if change.GetBlock() {
			return change, nil
		}
		if change != nil && change.Amount != nil {
			hit.Amount = change.GetAmount()
		}
	}

	// Return an amount only when a handler changed it.
	if hit.GetAmount() == event.GetAmount() {
		return &DamageResult{}, nil
	}
	return &DamageResult{Amount: &hit.Amount}, nil
}

// Ping offers the ping to each ping handler, stopping at the first that
// blocks it; the others add the players they mute its voice line for.
func (h *Registry) Ping(event *PingEvent) (*PingResult, error) {
	// Keep the first occurrence of each muted slot in callback order.
	var muteVoice []int32
	muted := map[int32]struct{}{}

	// A block stops delivery; other results add their muted recipients.
	for _, handler := range h.pings {
		result := handler(event)
		if result.GetBlock() {
			return &PingResult{Block: true}, nil
		}
		for _, slot := range result.GetMuteVoice() {
			if _, seen := muted[slot]; !seen {
				muted[slot] = struct{}{}
				muteVoice = append(muteVoice, slot)
			}
		}
	}

	// Preserve the ordered recipient union in the reply.
	return &PingResult{MuteVoice: muteVoice}, nil
}

// Damaged runs the damaged handlers.
func (h *Registry) Damaged(event *DamagedEvent) error { return each(h.damageds, event) }

// Killed runs the kill handlers.
func (h *Registry) Killed(event *KillEvent) error { return each(h.kills, event) }

// AbilityCast runs the cast handlers.
func (h *Registry) AbilityCast(event *AbilityCastEvent) error { return each(h.casts, event) }

// Healed runs the heal handlers.
func (h *Registry) Healed(event *HealEvent) error { return each(h.heals, event) }

// LevelChanged runs the level handlers.
func (h *Registry) LevelChanged(event *LevelEvent) error { return each(h.levels, event) }

// Melee runs the melee handlers.
func (h *Registry) Melee(event *MeleeEvent) error { return each(h.melees, event) }

// Parried runs the parry handlers.
func (h *Registry) Parried(event *ParryEvent) error { return each(h.parries, event) }

// each calls every handler with the event.
func each[E any](handlers []func(E), event E) error {
	for _, handler := range handlers {
		handler(event)
	}
	return nil
}

// ModifierFired runs the handlers of the event's name.
func (h *Registry) ModifierFired(event *ModifierFiredEvent) error {
	for _, handler := range h.modifiers[event.GetName()] {
		handler(event)
	}
	return nil
}

// Input runs the input handlers.
func (h *Registry) Input(event *InputEvent) error {
	for _, handler := range h.inputs {
		handler(PlayerOf(event.GetPlayer()), event.GetPressed(), event.GetReleased())
	}
	return nil
}

// Restored runs the restored handlers.
func (h *Registry) Restored(event *RestoredEvent) error {
	err := errorOf(event.GetError())
	for _, handler := range h.restoreds {
		handler(PlayerOf(event.GetPlayer()), err)
	}
	return nil
}

// NpcsRestored runs the NPC restored handlers.
func (h *Registry) NpcsRestored(event *NpcsRestoredEvent) error {
	err := errorOf(event.GetError())
	for _, handler := range h.npcsRestoreds {
		handler(err)
	}
	return nil
}

// Launch runs the launch handlers.
func (h *Registry) Launch(event *LaunchEvent) error { return each(h.launches, event) }

// Impact runs the impact handlers.
func (h *Registry) Impact(event *ImpactEvent) error { return each(h.impacts, event) }

// Landed runs the landing handlers.
func (h *Registry) Landed(event *LandedEvent) error { return each(h.landeds, event) }

// SettingChanged runs the setting handlers.
func (h *Registry) SettingChanged(event *SettingChangedEvent) error {
	for _, handler := range h.settings {
		handler(PlayerOf(event.GetPlayer()), event.GetKey(), event.GetValue())
	}
	return nil
}

// PlayersChanged notifies roster watchers after the adapter publishes the list.
func (h *Registry) PlayersChanged(players []*Connection) error {
	return each(h.rosterWatches, players)
}

// PlayerJoined runs the join handlers.
func (h *Registry) PlayerJoined(event *PlayerJoinedEvent) error { return each(h.joins, event) }

// PlayerAway runs the away handlers.
func (h *Registry) PlayerAway(event *PlayerAwayEvent) error { return each(h.aways, event) }

// PlayerReturned runs the return handlers.
func (h *Registry) PlayerReturned(event *PlayerReturnedEvent) error {
	return each(h.returns, event)
}

// PlayerLeft runs the leave handlers at once, so the leaving player's hero
// still reads.
func (h *Registry) PlayerLeft(event *PlayerLeftEvent) error {
	for _, handler := range h.leaves {
		handler(event)
	}
	return nil
}

// watchesSpawns reports whether spawn delivery needs the game's modifier event.
func (r *Registry) watchesSpawns() bool { return len(r.spawns) != 0 }
