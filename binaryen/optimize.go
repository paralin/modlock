package binaryen

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"

	"github.com/pkg/errors"
)

// features are the WebAssembly features Optimize lets wasm-opt use: those a
// scriptc module already uses and the Modlock host accepts. Binaryen's own
// default of every feature writes encodings the host cannot read, such as
// compact imports, which left a mod's module unloadable.
var features = []string{
	"--enable-bulk-memory",
	"--enable-nontrapping-float-to-int",
	"--enable-sign-ext",
	"--enable-mutable-globals",
	"--enable-multivalue",
	"--enable-reference-types",
	"--enable-simd",
}

// Optimize returns wasm after wasm-opt's -O2 passes, which shrink a scriptc
// module by about a fifth in seconds. The passes keep the module's imports
// and exports.
func Optimize(ctx context.Context, wasm []byte) ([]byte, error) {
	// Install wasm-opt.
	tool, err := WasmOpt(ctx)
	if err != nil {
		return nil, err
	}

	// Write the module to a temporary directory.
	dir, err := os.MkdirTemp("", "modlock-wasm-opt-")
	if err != nil {
		return nil, errors.Wrap(err, "optimize the module")
	}
	defer os.RemoveAll(dir)
	input := filepath.Join(dir, "in.wasm")
	output := filepath.Join(dir, "out.wasm")
	if err := os.WriteFile(input, wasm, 0o644); err != nil {
		return nil, errors.Wrap(err, "optimize the module")
	}

	// Run wasm-opt over it.
	args := append([]string{"-O2"}, features...)
	cmd := exec.CommandContext(ctx, tool, append(args, input, "-o", output)...)
	cmd.Env = os.Environ()
	if _, set := os.LookupEnv("BINARYEN_CORES"); !set {
		// Binaryen's parallel -O2 passes race on large modules and crash
		// (segfaults, SIGFPE, assertion failures), more often under load.
		// One thread passes every run and makes the output reproducible.
		cmd.Env = append(cmd.Env, "BINARYEN_CORES=1")
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return nil, errors.Wrapf(err, "wasm-opt: %s", bytes.TrimSpace(stderr.Bytes()))
	}

	// Read the optimized module back.
	return os.ReadFile(output)
}
