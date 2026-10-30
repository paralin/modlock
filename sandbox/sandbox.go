package sandbox

import (
	"context"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/paralin/modlock/files"
	"github.com/paralin/modlock/metrics"
	"github.com/paralin/modlock/settings"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm"
	"github.com/paralin/modlock/src/wasm/effects"
	filespb "github.com/paralin/modlock/src/wasm/files"
	"github.com/paralin/modlock/src/wasm/handoff"
	manifestpb "github.com/paralin/modlock/src/wasm/manifest"
	metricspb "github.com/paralin/modlock/src/wasm/metrics"
	"github.com/paralin/modlock/src/wasm/players"
	settingspb "github.com/paralin/modlock/src/wasm/settings"
	"github.com/paralin/modlock/src/wasm/ui"
	"github.com/pkg/errors"
	"github.com/tetratelabs/wazero"
)

// frameInterval is the sandbox's frame clock.
const frameInterval = time.Second / 60

// player is the stand-in player every sandbox holds. It develops and tests
// the mod, as the local players of a play session do.
var player = &players.Connection{
	Player:     0,
	SteamId:    76561197960265728,
	Name:       "Player",
	Ready:      true,
	Generation: 1,
	Roles:      []players.ModRole{players.ModRole_MOD_ROLE_DEVELOPER, players.ModRole_MOD_ROLE_TESTER},
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
	// once, and once across sessions when the config names a directory.
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
	// settings keeps the player's settings.
	settings *settings.File
	// tallies holds the stand-in player's metric totals by mod, which the
	// session logs when it ends.
	tallies map[string]*metrics.Tally
	// handoff holds the handoff state of each mod for the game and for the
	// stand-in player, which the next build reads back after a reload.
	handoff map[handoffKey]string
}

// handoffLimit is the most handoff state a mod sets in one call, as a server
// allows.
const handoffLimit = 64 * 1024

// handoffKey names one mod's handoff state: the game's, or the player's.
type handoffKey struct {
	// mod is the mod's slug.
	mod string
	// player is set for the stand-in player's state.
	player bool
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
		cache:   compilationCache(config.Compiled),
		actions: make(chan func(context.Context)),
		stop:    stop,
		done:    make(chan struct{}),
		tallies: map[string]*metrics.Tally{},
	}
	go s.run(ctx)
	return s
}

// compilationCache returns a cache that keeps compiled modules in dir, or in
// memory when dir is empty or cannot hold them; a missing cache only costs
// compile time.
func compilationCache(dir string) wazero.CompilationCache {
	if dir != "" {
		if cache, err := wazero.NewCompilationCacheWithDir(dir); err == nil {
			return cache
		}
	}
	return wazero.NewCompilationCache()
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
				pressed = s.deliver(ctx, running, "UiPress", &ui.UiPressEvent{Player: slot, Node: node}, nil)
			}
		}
	})
	return pressed, err
}

// Input delivers a change to an input in the interface mod shows the
// event's player, and reports whether the mod is running to receive it. The
// creator's own preview sends it, so the sandbox passes the value on as sent.
func (s *Sandbox) Input(ctx context.Context, mod string, event *ui.UiInputEvent) (bool, error) {
	var changed bool
	err := s.do(ctx, func(ctx context.Context) {
		for _, running := range s.mods {
			if running.name == mod {
				changed = s.deliver(ctx, running, "UiInput", event, nil)
			}
		}
	})
	return changed, err
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

	// Read the player's settings; a file that does not read starts empty.
	var err error
	if s.settings, err = settings.Open(s.config.Settings); err != nil {
		s.log("", "settings: "+err.Error()+"; starting without them")
	}

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
		Calls:  func(call *wasm.Call) *wasm.Reply { return s.answer(build, call) },
		Output: &lines{write: func(line string) { s.log(name, line) }},
		Cache:  s.cache,
	})
	if err != nil {
		return nil, nil, err
	}

	// Start it, report its one player as a game server does before the next
	// event, then give it the world. A build from before the report answers
	// that it has no such method and keeps asking for its players.
	wants := &wasm.StartResult{}
	err = mod.Deliver(ctx, "Start", &wasm.StartEvent{Args: s.config.Args, Source: source}, wants)
	if err == nil {
		_ = mod.Deliver(ctx, "PlayersChanged", &players.PlayersChangedEvent{Players: []*players.Connection{player}}, nil)
	}
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
	if runtime == manifestpb.Manifest_RUNTIME_WASM {
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
// the event. A slow event is logged, as the host warns of it. A mod that traps
// or runs out of time stops and waits for its next build, as the host's waits
// for a player's restart.
func (s *Sandbox) deliver(ctx context.Context, mod *running, method string, request, response message) bool {
	// Skip a mod that stopped.
	if mod.mod == nil {
		return false
	}

	// Deliver the event; an error from a closed instance stops the mod.
	began := time.Now()
	err := mod.mod.Deliver(ctx, method, request, response)
	if took := time.Since(began); took >= slowEvent {
		s.log(mod.name, "slow: "+method+" took "+strconv.FormatInt(took.Milliseconds(), 10)+" ms")
	}
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

// answer answers a host call from the mod manifest describes. It reports
// what the player would see or what the server log would show, names the
// stand-in player, keeps the player's settings, refuses service calls as a
// host without extensions does, and answers the rest empty, as for a player
// without a hero.
func (s *Sandbox) answer(build *Build, call *wasm.Call) *wasm.Reply {
	// Decode the requests the sandbox reports.
	manifest := build.Manifest
	mod := manifest.GetSlug()
	switch call.GetMethod() {
	case "Log":
		request := &wasm.LogRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.log(mod, request.GetMessage())
		}
	case "Chat":
		request := &effects.ChatRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.message(mod, request.GetPlayer(), control.MessageKind_MESSAGE_KIND_CHAT, "", request.GetText())
		}
	case "CenterText":
		request := &effects.CenterTextRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.message(mod, request.GetPlayer(), control.MessageKind_MESSAGE_KIND_CENTER, "", request.GetText())
		}
	case "Announce":
		request := &effects.AnnounceRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.message(mod, request.GetPlayer(), control.MessageKind_MESSAGE_KIND_ANNOUNCEMENT, request.GetTitle(), request.GetText())
		}
	case "ServerCommand":
		request := &wasm.ServerCommandRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.log(mod, "server command: "+request.GetCommand())
		}
	case "Ui":
		request := &ui.UiRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.event(&control.HostEvent{Body: &control.HostEvent_Ui{Ui: &control.UiChanged{
				Mod:    mod,
				Slot:   request.GetPlayer(),
				Change: request.GetChange(),
			}}})
		}
	case "Players":
		response, _ := (&players.PlayersResponse{Players: []*players.Connection{player}}).MarshalVT()
		return &wasm.Reply{Response: response}
	case "PlayerSetting":
		request := &settingspb.PlayerSettingRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			response, _ := (&settingspb.SettingResponse{Value: s.setting(manifest, request.GetPlayer(), request.GetKey())}).MarshalVT()
			return &wasm.Reply{Response: response}
		}
	case "SetPlayerSetting":
		request := &settingspb.SetPlayerSettingRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.setSetting(manifest, request.GetPlayer(), request.GetKey(), request.GetValue())
		}
	case "AddMetric":
		request := &metricspb.AddMetricRequest{}
		if request.UnmarshalVT(call.GetRequest()) == nil {
			s.addMetric(manifest, request)
		}
	case "Handoff", "HandedOff", "PlayerHandoff", "PlayerHandedOff":
		return s.handOff(mod, call)
	case "ReadFile", "WriteFile":
		return s.file(build, call)
	case "CallService":
		request := &wasm.ServiceCall{}
		_ = request.UnmarshalVT(call.GetRequest())
		return &wasm.Reply{Error: "this host provides no service " + request.GetService()}
	}

	return &wasm.Reply{}
}

// handOff answers a call that sets or reads the handoff state of mod for the
// game or the stand-in player. A bot's slot has none.
func (s *Sandbox) handOff(mod string, call *wasm.Call) *wasm.Reply {
	// Find the state the call names.
	key := handoffKey{mod: mod}
	var slot int32
	var text string
	sets := true
	switch call.GetMethod() {
	case "Handoff":
		request := &handoff.HandoffRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the Handoff request is malformed"}
		}
		text, slot = request.GetText(), player.GetPlayer()
	case "PlayerHandoff":
		request := &handoff.HandoffPlayerRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the PlayerHandoff request is malformed"}
		}
		key.player, text, slot = true, request.GetText(), request.GetPlayer()
	case "HandedOff":
		sets, slot = false, player.GetPlayer()
	case "PlayerHandedOff":
		request := &players.PlayerRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the PlayerHandedOff request is malformed"}
		}
		key.player, sets, slot = true, false, request.GetPlayer()
	}

	// Set or read it, as a server does for one player.
	if sets {
		if len(text) > handoffLimit {
			return &wasm.Reply{Error: "handoff state is limited to 64 KiB"}
		}
		if slot != player.GetPlayer() {
			return &wasm.Reply{}
		}
		if s.handoff == nil {
			s.handoff = map[handoffKey]string{}
		}
		if text == "" {
			delete(s.handoff, key)
		} else {
			s.handoff[key] = text
		}
		return &wasm.Reply{}
	}
	response := &handoff.HandedOffResponse{}
	if slot == player.GetPlayer() {
		response.Text = s.handoff[key]
	}
	data, _ := response.MarshalVT()
	return &wasm.Reply{Response: data}
}

// file answers a call that reads or writes one of build's files, kept in a
// directory named by its slug.
func (s *Sandbox) file(build *Build, call *wasm.Call) *wasm.Reply {
	// Find the mod's files.
	mod := &files.Mod{Data: build.Data}
	if s.config.Files != "" {
		mod.Written = filepath.Join(s.config.Files, build.Manifest.GetSlug())
	}

	// Write the file, or read it and leave the text unset when it is missing.
	if call.GetMethod() == "WriteFile" {
		request := &filespb.WriteFileRequest{}
		if err := request.UnmarshalVT(call.GetRequest()); err != nil {
			return &wasm.Reply{Error: "the WriteFile request is malformed"}
		}
		if err := mod.Write(request.GetPath(), request.GetText()); err != nil {
			return &wasm.Reply{Error: err.Error()}
		}
		return &wasm.Reply{}
	}
	request := &filespb.ReadFileRequest{}
	if err := request.UnmarshalVT(call.GetRequest()); err != nil {
		return &wasm.Reply{Error: "the ReadFile request is malformed"}
	}
	text, found, err := mod.Read(request.GetPath())
	if err != nil {
		return &wasm.Reply{Error: err.Error()}
	}
	response := &filespb.ReadFileResponse{}
	if found {
		response.Text = &text
	}
	data, _ := response.MarshalVT()
	return &wasm.Reply{Response: data}
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

// setting returns the player's value of a setting manifest declares: the
// kept value while the setting accepts it, or its default. An undeclared
// setting reads empty.
func (s *Sandbox) setting(manifest *manifestpb.Manifest, slot int32, key string) string {
	declared := settings.Find(manifest, key)
	if declared == nil {
		return ""
	}
	if slot == player.GetPlayer() {
		if kept, ok := s.settings.Value(manifest.GetSlug(), player.GetSteamId(), key); ok {
			if value, ok := settings.Canonical(declared, kept); ok {
				return value
			}
		}
	}
	return settings.Default(declared)
}

// setSetting keeps the player's new value of a setting manifest declares,
// logging a value the setting refuses.
func (s *Sandbox) setSetting(manifest *manifestpb.Manifest, slot int32, key, value string) {
	// Accept only a value of a declared setting.
	mod := manifest.GetSlug()
	declared := settings.Find(manifest, key)
	if declared == nil {
		s.log(mod, "mod.json declares no setting "+key)
		return
	}
	canonical, ok := settings.Canonical(declared, value)
	if !ok {
		s.log(mod, value+" is not a value of "+declared.GetLabel())
		return
	}

	// Keep it for the stand-in player; another slot is a bot's.
	if slot != player.GetPlayer() {
		return
	}
	if err := s.settings.Store(mod, player.GetSteamId(), key, canonical); err != nil {
		s.log(mod, "settings: "+err.Error())
	}
}

// addMetric adds to the stand-in player's total of a metric manifest
// declares, logging a call the declaration refuses.
func (s *Sandbox) addMetric(manifest *manifestpb.Manifest, request *metricspb.AddMetricRequest) {
	// Count only a declared metric and label.
	mod := manifest.GetSlug()
	declared := metrics.Find(manifest, request.GetName())
	if declared == nil {
		s.log(mod, "mod.json declares no metric "+request.GetName())
		return
	}
	if !metrics.Labeled(declared, request.GetLabel()) {
		s.log(mod, request.GetLabel()+" is not a label of metric "+request.GetName())
		return
	}

	// Keep it for the stand-in player; another slot is a bot's.
	if request.GetPlayer() != player.GetPlayer() {
		return
	}
	if s.tallies[mod] == nil {
		s.tallies[mod] = &metrics.Tally{}
	}
	s.tallies[mod].Add(declared, request.GetLabel(), request.GetValue())
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
	// Report the player's metrics as the mods stop, as a server does when
	// the player leaves.
	for _, mod := range s.mods {
		if tally := s.tallies[mod.name]; tally != nil {
			s.log(mod.name, "metrics for "+strconv.FormatUint(player.GetSteamId(), 10)+": "+tally.Text())
		}
	}
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
