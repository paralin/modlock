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

// defaultRegistry is the module's registration target for package functions.
var defaultRegistry Registry

// Command calls handler when a player types /name in chat. The handler runs
// with the next server frame, before the frame handlers, and receives the
// text after the name, trimmed of surrounding spaces. Registering a name
// again replaces its handler.
func Command(name string, handler func(p Player, args string)) {
	defaultRegistry.RegisterCommand(name, handler)
}

// Serve answers the host's calls to service with handler. Registering a
// service again replaces its handler.
func Serve(service string, handler Service) {
	defaultRegistry.RegisterService(service, handler)
}

// OnStart calls handler once when the server starts the mod, with the
// words after -- on the modlock dev or play command line.
func OnStart(handler func(args []string)) {
	defaultRegistry.OnStart(handler)
}

// OnFrame calls handler once per server frame.
func OnFrame(handler func(f *FrameEvent)) {
	defaultRegistry.OnFrame(handler)
}

// OnChat calls handler with each chat line a player writes, commands
// included, on the next server frame.
func OnChat(handler func(line *ChatEvent)) {
	defaultRegistry.OnChat(handler)
}

// OnPlayerJoin calls handler once for each player the mod serves when they
// are ready: a human who finished joining, a bot once added, and at start
// each player already in the game.
func OnPlayerJoin(handler func(joined *PlayerJoinedEvent)) {
	defaultRegistry.OnPlayerJoin(handler)
}

// OnPlayerAway calls handler once each time a joined human's client
// disconnects while the server holds their seat. They have not left: their
// hero and controller stay in the game, and OnPlayerReturn follows if they
// come back, so keep their seat as it stands rather than clear it.
func OnPlayerAway(handler func(away *PlayerAwayEvent)) {
	defaultRegistry.OnPlayerAway(handler)
}

// OnPlayerReturn calls handler once each time an away player reconnects to
// their seat and finishes joining again, with the same Player, generation and
// hero they left.
func OnPlayerReturn(handler func(returned *PlayerReturnedEvent)) {
	defaultRegistry.OnPlayerReturn(handler)
}

// OnPlayerLeave calls handler once for each joined player who leaves the mod,
// with the reason the server dropped them when it gave one. A seat ends, and
// its player leaves, when a bot is removed, a human is kicked or released by
// the program that runs the host, or a human stays away past the game's
// limit; a human who only disconnects is away first. The slot and hero still
// read during the call, and afterwards Modlock kills the hero, keeps it down
// and takes the controller off its team, so the mod need not clear them. A
// player the mod stops serving leaves too, with Disconnected false.
func OnPlayerLeave(handler func(left *PlayerLeftEvent)) {
	defaultRegistry.OnPlayerLeave(handler)
}

// OnWorld calls handler with the map's name each time a world loads, and at
// start when one already has. Objects and bots from an earlier world are gone
// by then.
func OnWorld(handler func(mapName string)) {
	defaultRegistry.OnWorld(handler)
}

// OnInput calls handler with the watched buttons a player pressed and
// released, as Button bits, before the next frame. A button is held from its
// press until its release.
func OnInput(handler func(p Player, pressed, released uint64)) {
	defaultRegistry.OnInput(handler)
}

// OnRestored calls handler when a player's Player.RestoreHero ends: err is
// nil once the target held for a second, or says why it did not.
func OnRestored(handler func(p Player, err error)) {
	defaultRegistry.OnRestored(handler)
}

// OnNpcsRestored calls handler when RestoreNpcs ends: err is nil once the map
// holds every target, or says why it does not.
func OnNpcsRestored(handler func(err error)) {
	defaultRegistry.OnNpcsRestored(handler)
}

// OnDamage calls handler before each hit lands, so it can block the hit or
// change its damage by returning a result; nil lets the hit through. Later
// handlers see the earlier ones' amount.
func OnDamage(handler func(hit *DamageEvent) *DamageResult) {
	defaultRegistry.OnDamage(handler)
}

// OnPing calls handler with each ping a player sends, from the comms wheel, a
// quick ping key, the map or an ability, before anyone sees it, so it can
// block the ping or mute its voice line. A blocked ping reaches nobody, the
// sender included: no marker, sound, voice line or chat line. The players a
// handler lists in MuteVoice, by slot, get the ping without its voice line.
// The handler runs inside the server's ping, so it decides at once and defers
// other work to a frame.
func OnPing(handler func(ping *PingEvent) *PingResult) {
	defaultRegistry.OnPing(handler)
}

// OnDamaged calls handler on the frame after each hit lands.
func OnDamaged(handler func(hit *DamagedEvent)) {
	defaultRegistry.OnDamaged(handler)
}

// OnKill calls handler after each hit that kills a hero or a unit, before the
// next frame. The kill names both entities and the player of each: a slot of
// -1 is a unit's, or a hazard's for the attacker.
func OnKill(handler func(kill *KillEvent)) {
	defaultRegistry.OnKill(handler)
}

// OnAbilityCast calls handler after each ability the game executes, before
// the next frame: a hero's abilities and the items a player activates, at
// the moment the cast takes effect.
func OnAbilityCast(handler func(cast *AbilityCastEvent)) {
	defaultRegistry.OnAbilityCast(handler)
}

// OnHeal calls handler after each heal, before the next frame: health a hero
// or unit regained at once from an ability, an item or lifesteal. Regeneration
// calls nothing, including what abilities such as Siphon Life restore as
// regeneration, and neither do the mod's own heal and a heal at full health.
// The heal names the entity healed, the healer and ability when the game
// names them, and the player of each: a slot of -1 is a unit's, or no one's.
func OnHeal(handler func(heal *HealEvent)) {
	defaultRegistry.OnHeal(handler)
}

// OnLevelChange calls handler as each hero reaches a new level, before the
// next frame.
func OnLevelChange(handler func(change *LevelEvent)) {
	defaultRegistry.OnLevelChange(handler)
}

// OnMelee calls handler after each melee swing, before the next frame, with
// whether it was heavy and the entities it struck, none for a miss.
func OnMelee(handler func(swing *MeleeEvent)) {
	defaultRegistry.OnMelee(handler)
}

// OnParry calls handler after each parry that catches a melee swing, before
// the next frame, naming the hero that parried and the attacker it caught.
func OnParry(handler func(parry *ParryEvent)) {
	defaultRegistry.OnParry(handler)
}

// OnModifierEvent calls handler each time the game fires the modifier event
// with this EModifierEvent name, such as ABILITY_CAST_STARTED, PARRY_SUCCESS
// or PLAYER_RESPAWNED, before the next frame. Register before the mod
// starts. A name the running game lacks is logged once and never fires.
func OnModifierEvent(name string, handler func(event *ModifierFiredEvent)) {
	defaultRegistry.OnModifierEvent(name, handler)
}

// OnLaunch calls handler with each watched projectile's first frame, before
// the next frame. WatchProjectiles names the projectiles.
func OnLaunch(handler func(projectile *LaunchEvent)) {
	defaultRegistry.OnLaunch(handler)
}

// OnImpact calls handler when a watched projectile strikes something, inside
// the game's impact: its calls apply before the game continues.
func OnImpact(handler func(impact *ImpactEvent)) {
	defaultRegistry.OnImpact(handler)
}

// OnLanded calls handler when a hero lands under the manifest's QuakeWorld
// movement, before the next frame.
func OnLanded(handler func(landing *LandedEvent)) {
	defaultRegistry.OnLanded(handler)
}

// OnSettingChanged calls handler when a player's setting changes outside the
// mod, such as on the player's profile. The mod's own Player.SetSetting calls
// do not reach it.
func OnSettingChanged(handler func(p Player, key, value string)) {
	defaultRegistry.OnSettingChanged(handler)
}

// OnUiPress calls handler with each press a player makes in the mod's
// interface, with the next server frame: a button's, or a layout's with the
// action its bound panel or its script names. Package ui registers it to run
// the buttons' and the bindings' handlers.
func OnUiPress(handler func(event *UiPressEvent)) {
	defaultRegistry.OnUiPress(handler)
}

// OnUiInput calls handler with each change a player makes to an input in the
// mod's interface, with the next server frame. Package ui registers it to run
// the inputs' handlers.
func OnUiInput(handler func(event *UiInputEvent)) {
	defaultRegistry.OnUiInput(handler)
}

// OnUiDocument calls handler with the end of each submission from a player's
// document input, with the next server frame: the whole document, or why it
// did not arrive.
func OnUiDocument(handler func(event *UiDocumentEvent)) {
	defaultRegistry.OnUiDocument(handler)
}

// OnUiAction calls handler with each phase of a game action the root of a
// player's interface follows, with the next server frame. Package ui
// registers it to run the followers' handlers.
func OnUiAction(handler func(event *UiActionEvent)) {
	defaultRegistry.OnUiAction(handler)
}

// OnUiOverlay calls handler with each stock overlay that takes or gives back
// the input of a player whose interface root follows overlays, with the next
// server frame. Package ui registers it to run the followers' handlers.
func OnUiOverlay(handler func(event *UiOverlayEvent)) {
	defaultRegistry.OnUiOverlay(handler)
}

// OnUiPlacement calls handler with each placement of the interface of a
// player whose interface root follows placement, with the next server frame.
// Package ui registers it to run the followers' handlers.
func OnUiPlacement(handler func(event *UiPlacementEvent)) {
	defaultRegistry.OnUiPlacement(handler)
}

// OnUiWindow calls handler with each run of a list's rows a player's screen
// asks the mod to show, with the next server frame. Package ui registers it
// to draw the window it asks for.
func OnUiWindow(handler func(event *UiWindowEvent)) {
	defaultRegistry.OnUiWindow(handler)
}

// OnPlayers calls handler with each player list the host reports, before the
// event it arrives with. A slot whose generation changed holds a new
// connection.
func OnPlayers(handler func(players []*Connection)) {
	defaultRegistry.OnPlayers(handler)
}
