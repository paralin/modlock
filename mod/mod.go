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
	"strings"
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
	worlds        []func(mapName string)
	inputs        []func(p Player, pressed, released uint64)
	restoreds     []func(p Player, err error)
	npcsRestoreds []func(err error)
	damages       []func(*DamageEvent) *DamageResult
	damageds      []func(*DamagedEvent)
	launches      []func(*LaunchEvent)
	impacts       []func(*ImpactEvent)
	landeds       []func(*LandedEvent)

	// result holds the last encoded Reply until the host has copied it.
	result []byte
}

// registered is the mod's single set of handlers.
var registered = &handlers{commands: map[string]func(Player, string){}, services: map[string]Service{}}

// Command calls handler when a player types /name in chat. The handler
// receives the text after the name, trimmed of surrounding spaces.
// Registering a name again replaces its handler.
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

// Start runs the start handlers and asks for the events the mod handles.
func (h *handlers) Start(event *StartEvent) (*StartResult, error) {
	for _, handler := range h.starts {
		handler(event.GetArgs())
	}
	return &StartResult{Frames: len(h.frames) != 0, Damage: len(h.damages) != 0, Damaged: len(h.damageds) != 0}, nil
}

// Frame runs the frame handlers.
func (h *handlers) Frame(event *FrameEvent) error {
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

	// Run the handler and claim the command.
	handler(Player{Slot: event.GetPlayer()}, strings.TrimSpace(args))
	return &CommandResult{Claimed: true}, nil
}

// World runs the world handlers.
func (h *handlers) World(event *WorldEvent) error {
	for _, handler := range h.worlds {
		handler(event.GetMap())
	}
	return nil
}

// UiPress ignores presses; Go mods build no interface.
func (h *handlers) UiPress(*UiPressEvent) error {
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

// Input runs the input handlers.
func (h *handlers) Input(event *InputEvent) error {
	for _, handler := range h.inputs {
		handler(Player{Slot: event.GetPlayer()}, event.GetPressed(), event.GetReleased())
	}
	return nil
}

// Restored runs the restored handlers.
func (h *handlers) Restored(event *RestoredEvent) error {
	err := errorOf(event.GetError())
	for _, handler := range h.restoreds {
		handler(Player{Slot: event.GetPlayer()}, err)
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

// errorOf returns message as an error, or nil when it is empty.
func errorOf(message string) error {
	if message == "" {
		return nil
	}
	return errors.New(message)
}
