// Package check decides whether a built mod may run, without a game. It is
// the check a publishing service runs before it offers a release to players:
// a WebAssembly mod must keep to the host's imports and start in the host's
// sandbox limits, and an interpreted mod must name a runtime Modlock ships.
package check

import (
	"context"
	"io/fs"
	"regexp"
	"time"

	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
	"github.com/tetratelabs/wazero"
	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/imports/wasi_snapshot_preview1"
)

// ManifestFile is the manifest's file name in a built mod.
const ManifestFile = "mod.json"

// The limits match modlock-host's sandbox.
const (
	// memoryBytes caps the mod's linear memory.
	memoryBytes = 256 << 20
	// startBudget bounds module initialization.
	startBudget = 5 * time.Second
	// eventBudget bounds the start event.
	eventBudget = 100 * time.Millisecond
)

// slugPattern matches a valid slug.
var slugPattern = regexp.MustCompile(`^[a-z0-9]+(-[a-z0-9]+)*$`)

// Slug reports whether slug is a valid mod slug.
func Slug(slug string) error {
	if !slugPattern.MatchString(slug) {
		return errors.Errorf("the slug %q must be lowercase letters, digits and single hyphens", slug)
	}
	return nil
}

// Mod checks the built mod in dist and returns its manifest.
func Mod(ctx context.Context, dist fs.FS) (*wasm.Manifest, error) {
	// Read the manifest and the entry it names.
	data, err := fs.ReadFile(dist, ManifestFile)
	if err != nil {
		return nil, errors.Wrap(err, "read mod.json")
	}
	manifest := &wasm.Manifest{}
	if err := manifest.UnmarshalJSON(data); err != nil {
		return nil, errors.Wrap(err, "parse mod.json")
	}
	if err := Slug(manifest.GetSlug()); err != nil {
		return nil, errors.Wrap(err, "mod.json")
	}
	if manifest.GetVersion() == "" {
		return nil, errors.New("mod.json names no version")
	}
	entry := manifest.GetEntry()
	if !fs.ValidPath(entry) || entry == "." {
		return nil, errors.Errorf("mod.json entry %q is not a file in the mod", entry)
	}
	source, err := fs.ReadFile(dist, entry)
	if err != nil {
		return nil, errors.Wrap(err, "read the entry")
	}

	// Run a module; an interpreted mod's runtime is Modlock's own.
	switch manifest.GetRuntime() {
	case wasm.Manifest_RUNTIME_WASM:
		return manifest, Module(ctx, source)
	case wasm.Manifest_RUNTIME_QUICKJS, wasm.Manifest_RUNTIME_LUAU, wasm.Manifest_RUNTIME_PYTHON:
		return manifest, nil
	default:
		return nil, errors.Errorf("mod.json names the runtime %s, which this Modlock does not run", manifest.GetRuntime())
	}
}

// Module checks that module imports only from modlock and WASI preview 1,
// exports memory and modlock_event, and starts within the host's memory and
// time limits with every host request answered empty.
func Module(ctx context.Context, module []byte) error {
	runtime := wazero.NewRuntimeWithConfig(ctx, wazero.NewRuntimeConfig().
		WithMemoryLimitPages(memoryBytes/65536).
		WithCloseOnContextDone(true))
	defer runtime.Close(ctx)

	// Compile the module and check its boundary.
	compiled, err := runtime.CompileModule(ctx, module)
	if err != nil {
		return errors.Wrap(err, "the entry is not a valid WebAssembly module")
	}
	if err := boundary(compiled); err != nil {
		return err
	}

	// Link WASI, with no files, environment or arguments, and the host stubs.
	wasi_snapshot_preview1.MustInstantiate(ctx, runtime)
	var host stubs
	_, err = runtime.NewHostModuleBuilder("modlock").
		NewFunctionBuilder().WithFunc(host.call).Export("host_call").
		NewFunctionBuilder().WithFunc(host.read).Export("host_read").
		Instantiate(ctx)
	if err != nil {
		return errors.Wrap(err, "link the host")
	}

	// Instantiate and initialize the mod within the start budget.
	started, cancel := context.WithTimeout(ctx, startBudget)
	defer cancel()
	instance, err := runtime.InstantiateModule(started, compiled, wazero.NewModuleConfig().WithStartFunctions())
	if err != nil {
		return errors.Wrap(err, "instantiate the mod")
	}
	if initialize := instance.ExportedFunction("_initialize"); initialize != nil {
		if _, err := initialize.Call(started); err != nil {
			return failure(started, err, "initialize", startBudget)
		}
	}

	// Deliver the start event within the event budget.
	start, err := (&wasm.StartEvent{CheckOnly: true}).MarshalVT()
	if err != nil {
		return err
	}
	event, err := (&wasm.Call{Method: "Start", Request: start}).MarshalVT()
	if err != nil {
		return err
	}
	host.pending = event
	delivered, cancel := context.WithTimeout(ctx, eventBudget)
	defer cancel()
	results, err := instance.ExportedFunction("modlock_event").Call(delivered, uint64(len(event)))
	if err != nil {
		return failure(delivered, err, "start", eventBudget)
	}
	return reply(instance.Memory(), results[0])
}

// boundary checks the module's imports and exports against the host's.
func boundary(compiled wazero.CompiledModule) error {
	for _, function := range compiled.ImportedFunctions() {
		module, name, _ := function.Import()
		switch {
		case module == wasi_snapshot_preview1.ModuleName:
		case module == "modlock" && (name == "host_call" || name == "host_read"):
		default:
			return errors.Errorf("the mod imports %s.%s, which the host does not offer", module, name)
		}
	}
	if len(compiled.ImportedMemories()) != 0 {
		return errors.New("the mod imports a memory; it must define its own")
	}
	if _, ok := compiled.ExportedMemories()["memory"]; !ok {
		return errors.New("the mod must export memory")
	}
	event, ok := compiled.ExportedFunctions()["modlock_event"]
	if !ok || !types(event.ParamTypes(), api.ValueTypeI32) || !types(event.ResultTypes(), api.ValueTypeI64) {
		return errors.New("the mod must export modlock_event taking i32 and returning i64")
	}
	return nil
}

// types reports whether got is exactly want.
func types(got []api.ValueType, want ...api.ValueType) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

// failure explains an error from the mod's step, naming the budget when
// the step ran out of time.
func failure(ctx context.Context, err error, step string, budget time.Duration) error {
	if ctx.Err() != nil {
		return errors.Errorf("the mod's %s took longer than %s", step, budget)
	}
	return errors.Wrapf(err, "the mod failed to %s", step)
}

// reply checks the Reply to the start event that the mod left in memory.
func reply(memory api.Memory, packed uint64) error {
	if packed == 0 {
		return nil
	}
	data, ok := memory.Read(uint32(packed>>32), uint32(packed))
	if !ok {
		return errors.New("modlock_event returned a reply outside memory")
	}
	answer := &wasm.Reply{}
	if err := answer.UnmarshalVT(data); err != nil {
		return errors.Wrap(err, "modlock_event returned an invalid Reply")
	}
	if message := answer.GetError(); message != "" {
		return errors.Errorf("the mod failed to start: %s", message)
	}
	return nil
}

// stubs answer the mod's host calls as a host with no game would.
type stubs struct {
	// pending is the message host_read copies out next.
	pending []byte
}

// call answers every call with an empty reply.
func (s *stubs) call(_ context.Context, _ api.Module, _, _ uint32) uint32 {
	s.pending = nil
	return 0
}

// read copies the pending message into the mod's memory, trapping on a size
// or range the host would refuse.
func (s *stubs) read(_ context.Context, mod api.Module, data, size uint32) {
	if int(size) != len(s.pending) {
		panic(errors.New("modlock.host_read: the size differs from the pending message"))
	}
	if !mod.Memory().Write(data, s.pending) {
		panic(errors.New("modlock.host_read: the buffer is outside memory"))
	}
	s.pending = nil
}
