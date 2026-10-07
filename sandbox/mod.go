// Package sandbox runs built mods without a game. A Mod is one WebAssembly
// module behind the host boundary, kept to modlock-host's imports and limits;
// a Sandbox runs a session of mods with a stand-in player, as a server would.
package sandbox

import (
	"context"
	"io"
	"time"

	"github.com/paralin/modlock/proto/modlock/wasm"
	"github.com/pkg/errors"
	"github.com/tetratelabs/wazero"
	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/imports/wasi_snapshot_preview1"
)

// The limits match modlock-host's sandbox.
const (
	// memoryBytes caps the mod's linear memory.
	memoryBytes = 256 << 20
	// startBudget bounds module initialization and the Start event.
	startBudget = 5 * time.Second
	// eventBudget bounds one event, including its host calls.
	eventBudget = 500 * time.Millisecond
)

// message is a protobuf message with fast encoding.
type message interface {
	// MarshalVT encodes the message.
	MarshalVT() ([]byte, error)
	// UnmarshalVT decodes data into the message.
	UnmarshalVT(data []byte) error
}

// Mod is one instance of a mod's module. It handles one event at a time.
type Mod struct {
	// runtime holds the instance and closes it.
	runtime wazero.Runtime
	// instance is the instantiated module.
	instance api.Module
	// calls answers the mod's host calls.
	calls func(call *wasm.Call) *wasm.Reply
	// pending is the message host_read copies out next.
	pending []byte
}

// Load checks that module imports only from modlock and WASI preview 1 and
// exports memory and modlock_event, then instantiates and initializes it
// within the host's memory and time limits.
func Load(ctx context.Context, module []byte, options Options) (*Mod, error) {
	// Make a runtime with the host's limits that closes the module when an
	// event's budget runs out.
	config := wazero.NewRuntimeConfig().
		WithMemoryLimitPages(memoryBytes / 65536).
		WithCloseOnContextDone(true)
	if options.Cache != nil {
		config = config.WithCompilationCache(options.Cache)
	}
	m := &Mod{runtime: wazero.NewRuntimeWithConfig(ctx, config), calls: options.Calls}
	if m.calls == nil {
		m.calls = func(*wasm.Call) *wasm.Reply { return &wasm.Reply{} }
	}

	// Compile the module, check its boundary and instantiate it.
	if err := m.instantiate(ctx, module, options.Output); err != nil {
		_ = m.runtime.Close(ctx)
		return nil, err
	}
	return m, nil
}

// instantiate compiles, links and initializes module.
func (m *Mod) instantiate(ctx context.Context, module []byte, output io.Writer) error {
	// Compile the module and check its boundary.
	compiled, err := m.runtime.CompileModule(ctx, module)
	if err != nil {
		return errors.Wrap(err, "the entry is not a valid WebAssembly module")
	}
	if err := boundary(compiled); err != nil {
		return err
	}

	// Link WASI, with only output, and the host.
	wasi_snapshot_preview1.MustInstantiate(ctx, m.runtime)
	_, err = m.runtime.NewHostModuleBuilder("modlock").
		NewFunctionBuilder().WithFunc(m.call).Export("host_call").
		NewFunctionBuilder().WithFunc(m.read).Export("host_read").
		Instantiate(ctx)
	if err != nil {
		return errors.Wrap(err, "link the host")
	}

	// Instantiate and initialize the mod within the start budget.
	started, cancel := context.WithTimeout(ctx, startBudget)
	defer cancel()
	moduleConfig := wazero.NewModuleConfig().WithStartFunctions()
	if output != nil {
		moduleConfig = moduleConfig.WithStdout(output).WithStderr(output)
	}
	m.instance, err = m.runtime.InstantiateModule(started, compiled, moduleConfig)
	if err != nil {
		return errors.Wrap(err, "instantiate the mod")
	}
	if initialize := m.instance.ExportedFunction("_initialize"); initialize != nil {
		if _, err := initialize.Call(started); err != nil {
			return failure(started, err, "initialize", startBudget)
		}
	}
	return nil
}

// Deliver calls the mod's method with request within the event budget and
// decodes its answer into response, which is nil for a method that answers
// nothing. Start gets the start budget instead, since a script mod evaluates
// its script there. An error the mod returns is the call's error; a trap or
// an exhausted budget also stops the mod.
func (m *Mod) Deliver(ctx context.Context, method string, request, response message) error {
	// Encode the call for the mod to read.
	data, err := request.MarshalVT()
	if err != nil {
		return err
	}
	m.pending, err = (&wasm.Call{Method: method, Request: data}).MarshalVT()
	if err != nil {
		return err
	}

	// Call the mod within its budget.
	budget := eventBudget
	if method == "Start" {
		budget = startBudget
	}
	delivered, cancel := context.WithTimeout(ctx, budget)
	defer cancel()
	results, err := m.instance.ExportedFunction("modlock_event").Call(delivered, uint64(len(m.pending)))
	if err != nil {
		_ = m.instance.Close(ctx)
		return failure(delivered, err, method, budget)
	}

	// Read the reply the mod left in its memory; none answers nothing.
	packed := results[0]
	if packed == 0 {
		return nil
	}
	data, ok := m.instance.Memory().Read(uint32(packed>>32), uint32(packed))
	if !ok {
		return errors.New("modlock_event returned a reply outside memory")
	}

	// Decode the reply into the mod's error or its response.
	reply := &wasm.Reply{}
	if err := reply.UnmarshalVT(data); err != nil {
		return errors.Wrap(err, "modlock_event returned an invalid Reply")
	}
	if text := reply.GetError(); text != "" {
		return errors.Errorf("the mod's %s failed: %s", method, text)
	}
	if response == nil {
		return nil
	}
	return response.UnmarshalVT(reply.GetResponse())
}

// Stopped reports whether a trap or an exhausted budget stopped the mod.
func (m *Mod) Stopped() bool {
	return m.instance.IsClosed()
}

// Close stops the mod.
func (m *Mod) Close(ctx context.Context) error {
	return m.runtime.Close(ctx)
}

// call answers one host call and holds the encoded reply for host_read.
func (m *Mod) call(_ context.Context, mod api.Module, data, size uint32) uint32 {
	// Decode the call from the mod's memory.
	bytes, ok := mod.Memory().Read(data, size)
	if !ok {
		panic(errors.New("modlock.host_call: the call is outside memory"))
	}
	call := &wasm.Call{}
	if err := call.UnmarshalVT(bytes); err != nil {
		panic(errors.New("modlock.host_call: invalid Call"))
	}

	// Answer it.
	reply, err := m.calls(call).MarshalVT()
	if err != nil {
		panic(errors.Wrap(err, "modlock.host_call"))
	}
	m.pending = reply
	return uint32(len(reply))
}

// read copies the pending message into the mod's memory, trapping on a size
// or range the host would refuse.
func (m *Mod) read(_ context.Context, mod api.Module, data, size uint32) {
	if int(size) != len(m.pending) {
		panic(errors.New("modlock.host_read: the size differs from the pending message"))
	}
	if !mod.Memory().Write(data, m.pending) {
		panic(errors.New("modlock.host_read: the buffer is outside memory"))
	}
	m.pending = nil
}

// boundary checks the module's imports and exports against the host's.
func boundary(compiled wazero.CompiledModule) error {
	// Allow only WASI and the host's two imports.
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

// failure explains an error from the mod's step, naming the budget when the
// step ran out of time.
func failure(ctx context.Context, err error, step string, budget time.Duration) error {
	if ctx.Err() != nil {
		return errors.Errorf("the mod's %s took longer than %s", step, budget)
	}
	return errors.Wrapf(err, "the mod failed to %s", step)
}
