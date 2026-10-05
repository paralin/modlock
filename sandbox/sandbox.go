package sandbox

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/paralin/modlock/proto/modlock/control"
	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
	"github.com/tetratelabs/wazero"
)

// frameInterval is the sandbox's frame clock.
const frameInterval = time.Second / 60

// player is the stand-in player every sandbox holds.
var player = &wasm.Connection{
	Player:     0,
	SteamId:    76561197960265728,
	Name:       "Player",
	Ready:      true,
	Generation: 1,
}

// Sandbox runs mods without a game, as a server would with one player who
// never has a hero: it starts each mod, gives it the world and the frame
// clock, passes the player's commands and button presses to it, and reports
// its logs, the messages it shows the player and its interface as host
// events. Host calls that need the game answer empty.
type Sandbox struct {
	// config describes the session.
	config Config
	// cache keeps compiled modules, so a reload and the interpreters compile
	// once.
	cache wazero.CompilationCache
	// actions carries work for the session's goroutine, which owns the mods.
	actions chan func(context.Context)
	// stop ends the session.
	stop context.CancelFunc
	// done is closed when the session ends.
	done chan struct{}
	// mods are the running mods, in load order.
	mods []*running
	// interpreters is the interpreters' directory once found.
	interpreters string
	// tick counts frames.
	tick uint64
}

// running is one mod in the session.
type running struct {
	// name is the mod's slug, which a reload matches.
	name string
	// mod is the running instance, or nil after it failed.
	mod *Mod
	// wants lists the events the mod consumes.
	wants *wasm.StartResult
}

// Start starts the mods in a new sandbox. A mod that fails to load reports
// the failure and leaves the others running.
func Start(ctx context.Context, config Config) *Sandbox {
	ctx, stop := context.WithCancel(ctx)
	s := &Sandbox{
		config:  config,
		cache:   wazero.NewCompilationCache(),
		actions: make(chan func(context.Context)),
		stop:    stop,
		done:    make(chan struct{}),
	}
	go s.run(ctx)
	return s
}

// Reload replaces the running mod with the build at path, or reports why it
// did not load and keeps the running one. It returns once the build loaded or
// failed to.
func (s *Sandbox) Reload(ctx context.Context, path string) error {
	return s.do(ctx, func(ctx context.Context) {
		s.load(ctx, path)
	})
}

// Command passes line to the mods as the player's command, as if typed in
// chat, and reports whether a mod claimed it. A leading slash is optional.
func (s *Sandbox) Command(ctx context.Context, line string) (bool, error) {
	var handled bool
	err := s.do(ctx, func(ctx context.Context) {
		handled = s.command(ctx, line)
	})
	return handled, err
}

// Press presses the button node in the interface mod shows the player in
// slot, and reports whether the mod is running to receive it.
func (s *Sandbox) Press(ctx context.Context, mod string, slot int32, node string) (bool, error) {
	var pressed bool
	err := s.do(ctx, func(ctx context.Context) {
		for _, running := range s.mods {
			if running.name == mod {
				pressed = s.deliver(ctx, running, "UiPress", &wasm.UiPressEvent{Player: slot, Node: node}, nil)
			}
		}
	})
	return pressed, err
}

// Wait waits for the session to end.
func (s *Sandbox) Wait() error {
	<-s.done
	return nil
}

// Stop ends the session.
func (s *Sandbox) Stop() {
	s.stop()
}

// run owns the mods: it starts them, then runs actions and frames until ctx
// ends.
func (s *Sandbox) run(ctx context.Context) {
	// Stop the mods and mark the session ended on return.
	defer close(s.done)
	defer s.close()

	// Start the world and its mods, then the player joins.
	s.event(&control.HostEvent{Body: &control.HostEvent_Ready{Ready: &control.ServerReady{Map: s.config.Map}}})
	for _, path := range s.config.Mods {
		s.load(ctx, path)
	}
	s.event(&control.HostEvent{Body: &control.HostEvent_Joined{Joined: &control.PlayerJoined{
		Slot:    player.GetPlayer(),
		Name:    player.GetName(),
		SteamId: player.GetSteamId(),
	}}})

	// Handle the session's requests and frames until it ends.
	clock := time.NewTicker(frameInterval)
	defer clock.Stop()
	started := time.Now()
	for {
		select {
		case <-ctx.Done():
			return
		case action := <-s.actions:
			action(ctx)
		case now := <-clock.C:
			s.frame(ctx, now.Sub(started))
		}
	}
}

// load starts the built mod at path, replacing the running mod of the same
// name once the new one starts.
func (s *Sandbox) load(ctx context.Context, path string) {
	// Read the build; a failure leaves the running mod as it is.
	name := filepath.Base(path)
	build, err := ReadBuild(os.DirFS(path))
	if err == nil {
		name = build.Manifest.GetSlug()
	}
	fail := func(err error) {
		if ctx.Err() != nil {
			return
		}
		s.event(&control.HostEvent{Body: &control.HostEvent_Failed{Failed: &control.ModFailed{
			Mod:   name,
			Error: err.Error(),
		}}})
	}
	if err != nil {
		fail(err)
		return
	}

	// Load its module and start it in the world.
	mod, wants, err := s.start(ctx, name, build)
	if err != nil {
		fail(err)
		return
	}

	// Replace the running mod of the same name, or add it.
	for _, old := range s.mods {
		if old.name == name {
			if old.mod != nil {
				_ = old.mod.Close(ctx)
			}
			old.mod, old.wants = mod, wants
			s.event(&control.HostEvent{Body: &control.HostEvent_Started{Started: &control.ModStarted{Mod: name, Reloaded: true}}})
			return
		}
	}
	s.mods = append(s.mods, &running{name: name, mod: mod, wants: wants})
	s.event(&control.HostEvent{Body: &control.HostEvent_Started{Started: &control.ModStarted{Mod: name}}})
}

// start loads build's module, starts it and gives it the world.
func (s *Sandbox) start(ctx context.Context, name string, build *Build) (*Mod, *wasm.StartResult, error) {
	// Choose the module: the mod's own, or the interpreter for its source.
	module, source, err := s.module(ctx, build)
	if err != nil {
		return nil, nil, err
	}

	// Load it with the sandbox answering its host calls.
	mod, err := Load(ctx, module, Options{
		Calls:  func(call *wasm.Call) *wasm.Reply { return s.answer(name, call) },
		Output: &lines{write: func(line string) { s.log(name, line) }},
		Cache:  s.cache,
	})
	if err != nil {
		return nil, nil, err
	}

	// Start it, then give it the world.
	wants := &wasm.StartResult{}
	err = mod.Deliver(ctx, "Start", &wasm.StartEvent{Args: s.config.Args, Source: source}, wants)
	if err == nil && s.config.Map != "" {
		err = mod.Deliver(ctx, "World", &wasm.WorldEvent{Map: s.config.Map}, nil)
	}
	if err != nil {
		_ = mod.Close(ctx)
		return nil, nil, err
	}
	return mod, wants, nil
}

// module returns the module that runs build and the source it evaluates,
// which is empty for a WebAssembly mod.
func (s *Sandbox) module(ctx context.Context, build *Build) ([]byte, []byte, error) {
	// A WebAssembly mod is its own module.
	runtime := build.Manifest.GetRuntime()
	if runtime == wasm.Manifest_RUNTIME_WASM {
		return build.Entry, nil, nil
	}

	// Read the interpreter for the mod's source.
	file := Interpreter(runtime)
	if file == "" {
		return nil, nil, errors.Errorf("mod.json names the runtime %s, which this Modlock does not run", runtime)
	}
	if s.interpreters == "" {
		dir, err := s.config.Interpreters(ctx)
		if err != nil {
			return nil, nil, errors.Wrap(err, "find the interpreters")
		}
		s.interpreters = dir
	}
	module, err := os.ReadFile(filepath.Join(s.interpreters, file))
	if err != nil {
		return nil, nil, errors.Wrap(err, "read the interpreter")
	}
	return module, build.Entry, nil
}

// command passes line to each mod in order until one claims it, and reports
// whether one did.
func (s *Sandbox) command(ctx context.Context, line string) bool {
	line = strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(line), "/"))
	if line == "" {
		return false
	}
	for _, mod := range s.mods {
		claimed := &wasm.CommandResult{}
		event := &wasm.CommandEvent{Player: player.GetPlayer(), Line: line}
		if s.deliver(ctx, mod, "Command", event, claimed) && claimed.GetClaimed() {
			return true
		}
	}
	return false
}

// frame gives one frame to each mod that consumes frames.
func (s *Sandbox) frame(ctx context.Context, elapsed time.Duration) {
	s.tick++
	for _, mod := range s.mods {
		if mod.wants.GetFrames() {
			s.deliver(ctx, mod, "Frame", &wasm.FrameEvent{Tick: s.tick, TimeSeconds: elapsed.Seconds()}, nil)
		}
	}
}

// deliver passes one event to a running mod and reports whether it handled
// the event. A mod that traps or runs out of time stops, as in the host, and
// waits for its next build.
func (s *Sandbox) deliver(ctx context.Context, mod *running, method string, request, response message) bool {
	// Skip a mod that stopped.
	if mod.mod == nil {
		return false
	}

	// Deliver the event; an error from a closed instance stops the mod.
	err := mod.mod.Deliver(ctx, method, request, response)
	if err == nil {
		return true
	}
	if ctx.Err() != nil {
		return false
	}
	s.event(&control.HostEvent{Body: &control.HostEvent_Failed{Failed: &control.ModFailed{Mod: mod.name, Error: err.Error()}}})
	if mod.mod.Stopped() {
		_ = mod.mod.Close(ctx)
		mod.mod = nil
	}
	return false
}

// answer answers a mod's host call. It reports what the player would see or
// what the server log would show, names the stand-in player, and answers the
// rest empty, as for a player without a hero.
func (s *Sandbox) answer(mod string, call *wasm.Call) *wasm.Reply {
	// Decode the requests the sandbox reports.
	switch call.GetMethod() {
	case "Log":
		request := &wasm.LogRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.log(mod, request.GetMessage())
		}
	case "Chat":
		request := &wasm.ChatRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.message(mod, request.GetPlayer(), control.MessageKind_MESSAGE_KIND_CHAT, "", request.GetText())
		}
	case "CenterText":
		request := &wasm.CenterTextRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.message(mod, request.GetPlayer(), control.MessageKind_MESSAGE_KIND_CENTER, "", request.GetText())
		}
	case "Announce":
		request := &wasm.AnnounceRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.message(mod, request.GetPlayer(), control.MessageKind_MESSAGE_KIND_ANNOUNCEMENT, request.GetTitle(), request.GetText())
		}
	case "ServerCommand":
		request := &wasm.ServerCommandRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.log(mod, "server command: "+request.GetCommand())
		}
	case "Ui":
		request := &wasm.UiRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.event(&control.HostEvent{Body: &control.HostEvent_Ui{Ui: &control.UiChanged{
				Mod:    mod,
				Slot:   request.GetPlayer(),
				Change: request.GetChange(),
			}}})
		}
	case "Players":
		response, _ := (&wasm.PlayersResponse{Players: []*wasm.Connection{player}}).MarshalVT()
		return &wasm.Reply{Response: response}
	}

	return &wasm.Reply{}
}

// message reports a message mod showed the player in slot.
func (s *Sandbox) message(mod string, slot int32, kind control.MessageKind, title, text string) {
	s.event(&control.HostEvent{Body: &control.HostEvent_Message{Message: &control.PlayerMessage{
		Mod:   mod,
		Slot:  slot,
		Kind:  kind,
		Title: title,
		Text:  text,
	}}})
}

// log reports one line the mod logged.
func (s *Sandbox) log(mod, text string) {
	s.event(&control.HostEvent{Body: &control.HostEvent_Log{Log: &control.ModLog{Mod: mod, Text: text}}})
}

// event reports one event.
func (s *Sandbox) event(event *control.HostEvent) {
	s.config.Events(event)
}

// close stops every mod and frees the compiled modules.
func (s *Sandbox) close() {
	ctx := context.Background()
	for _, mod := range s.mods {
		if mod.mod != nil {
			_ = mod.mod.Close(ctx)
		}
	}
	_ = s.cache.Close(ctx)
}

// do runs action on the session's goroutine and returns once it finished,
// unless ctx or the session ends first.
func (s *Sandbox) do(ctx context.Context, action func(context.Context)) error {
	done := make(chan struct{})
	run := func(ctx context.Context) {
		defer close(done)
		action(ctx)
	}
	select {
	case s.actions <- run:
		<-done
		return nil
	case <-s.done:
		return errors.New("the sandbox stopped")
	case <-ctx.Done():
		return context.Canceled
	}
}
