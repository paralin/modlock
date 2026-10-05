// Command modlock-sdkgen generates each language's mod library and the host's
// dispatch from the Host and Mod services in proto/modlock/wasm.proto, the
// single source of the boundary between the game and a mod. Run it from the
// repository root:
//
//	modlock-sdkgen
//
// It compiles the schema with the protoc that go-protoc-wasi runs in process,
// reads the descriptors with their comments, and writes:
//
//   - js/src/host.gen.ts: the TypeScript calls, classes, types and event
//     dispatch.
//   - mod/host.gen.go: the same for Go.
//   - luau/modlock/init.luau: the same for Luau, followed by the
//     handwritten luau/library.luau.
//   - python/modlock/__init__.py: the same for Python, followed by the
//     handwritten python/library.py.
//   - src/wasm/host_service.gen.h: the host's service with one virtual
//     method per call, its dispatch by method name, and the client that
//     delivers events to a mod.
//
// The conventions it follows are stated at the top of the schema.
package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"

	protoc "github.com/aperturerobotics/go-protoc-wasi"
	"github.com/aperturerobotics/protobuf-go-lite/types/descriptorpb"
	"github.com/tetratelabs/wazero"
)

// modulePath is the import root of the schema's files.
const modulePath = "github.com/paralin/modlock"

// schemaFile is the file that declares the services.
const schemaFile = modulePath + "/proto/modlock/wasm.proto"

// outputs are the generated files and the writer of each, relative to the
// repository root.
var outputs = []struct {
	path  string
	write func(*schema) ([]byte, error)
}{
	{"js/src/host.gen.ts", writeTypeScript},
	{"mod/host.gen.go", writeGo},
	{"luau/modlock/init.luau", writeLuau},
	{"python/modlock/__init__.py", writePython},
	{"src/wasm/host_service.gen.h", writeCpp},
}

func main() {
	if err := run(context.Background()); err != nil {
		fmt.Fprintln(os.Stderr, "modlock-sdkgen:", err)
		os.Exit(1)
	}
}

// run compiles the schema and writes every output.
func run(ctx context.Context) error {
	set, err := compile(ctx, ".")
	if err != nil {
		return err
	}
	s, err := readSchema(set, schemaFile)
	if err != nil {
		return err
	}
	for _, output := range outputs {
		data, err := output.write(s)
		if err != nil {
			return fmt.Errorf("%s: %w", output.path, err)
		}
		if err := os.WriteFile(filepath.FromSlash(output.path), data, 0o644); err != nil {
			return err
		}
	}
	return nil
}

// compile compiles the schema under root into a descriptor set holding its
// imports and comments.
func compile(ctx context.Context, root string) (*descriptorpb.FileDescriptorSet, error) {
	out, err := os.MkdirTemp("", "modlock-sdkgen-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(out)

	runtime := wazero.NewRuntime(ctx)
	defer runtime.Close(ctx)
	fsConfig := wazero.NewFSConfig().WithDirMount(out, "/").WithDirMount(root, "/"+modulePath)
	compiler, err := protoc.NewProtoc(ctx, runtime, &protoc.Config{FSConfig: fsConfig, Stderr: os.Stderr})
	if err != nil {
		return nil, err
	}
	defer compiler.Close(ctx)
	if err := compiler.Init(ctx); err != nil {
		return nil, err
	}
	code, err := compiler.Run(ctx, []string{
		"protoc", "-I/", "--include_imports", "--include_source_info",
		"--descriptor_set_out=/set.pb", "/" + schemaFile,
	})
	if err != nil {
		return nil, err
	}
	if code != 0 {
		return nil, fmt.Errorf("protoc exited with %d", code)
	}

	data, err := os.ReadFile(filepath.Join(out, "set.pb"))
	if err != nil {
		return nil, err
	}
	set := &descriptorpb.FileDescriptorSet{}
	if err := set.UnmarshalVT(data); err != nil {
		return nil, err
	}
	return set, nil
}
