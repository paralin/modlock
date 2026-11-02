// Package runtimes builds the interpreted runtimes' modules outside the C++
// build, for a command line that must run a script mod as the host does but
// has no host package at hand.
package runtimes

import (
	"bytes"
	"context"
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"os"
	"os/exec"
	"path/filepath"

	"github.com/paralin/modlock/internal/fetch"
	"github.com/pkg/errors"
)

// quickjsSource is the QuickJS runtime's own source and the boundary it
// includes, laid out as the runtime includes it.
//
//go:embed boundary.h quickjs/runtime.c
var quickjsSource embed.FS

// quickjsRelease is the QuickJS-ng release the C++ build compiles, and its
// archive's SHA-256; both match CMakeLists.txt.
const (
	// quickjsVersion is the release tag.
	quickjsVersion = "v0.17.0"
	// quickjsSum is the SHA-256 of the release's source archive.
	quickjsSum = "559bc4c420475e55c7ab4510adbc562f55d7524d75e8e89d79ce4bb02f5687d9"
)

// QuickJS returns quickjs.wasm compiled from this Modlock's runtime source with
// the zig on PATH, building it into the user cache the first time for each
// runtime source. The C++ build compiles the module the host ships with
// wasi-sdk instead; both compile the same sources with the same flags, so a
// mod runs the same on either.
func QuickJS(ctx context.Context) (string, error) {
	// Name the build by the runtime's sources.
	key := sha256.New()
	key.Write([]byte(quickjsSum))
	sources := []string{"boundary.h", "quickjs/runtime.c"}
	for _, name := range sources {
		data, err := quickjsSource.ReadFile(name)
		if err != nil {
			return "", err
		}
		key.Write(data)
	}
	dir, err := fetch.Cache("quickjs", hex.EncodeToString(key.Sum(nil))[:16])
	if err != nil {
		return "", err
	}

	// Compile it once.
	err = fetch.Install(dir, func(staging string) error {
		return buildQuickJS(ctx, staging, sources)
	})
	if err != nil {
		return "", errors.Wrap(err, "build the QuickJS runtime")
	}
	return filepath.Join(dir, "quickjs.wasm"), nil
}

// buildQuickJS compiles quickjs.wasm into dir from the QuickJS-ng release and
// the embedded runtime sources.
func buildQuickJS(ctx context.Context, dir string, sources []string) error {
	// Find the compiler first, so a missing one fails before any download.
	zig, err := exec.LookPath("zig")
	if err != nil {
		return errors.New("zig is not on PATH")
	}

	// Unpack QuickJS-ng and the runtime's sources beside dir.
	archive := dir + ".tar.gz"
	url := "https://github.com/quickjs-ng/quickjs/archive/refs/tags/" + quickjsVersion + ".tar.gz"
	if err := fetch.File(ctx, url, archive); err != nil {
		return err
	}
	if err := fetch.Verify(archive, quickjsSum); err != nil {
		return err
	}
	engine := dir + ".quickjs"
	if err := fetch.UntarTree(archive, "quickjs-"+quickjsVersion[1:]+"/", engine); err != nil {
		return err
	}
	runtime := dir + ".runtime"
	for _, name := range sources {
		data, _ := quickjsSource.ReadFile(name)
		path := filepath.Join(runtime, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(path, data, 0o644); err != nil {
			return err
		}
	}

	// Compile the reactor with the C++ build's flags.
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	var output bytes.Buffer
	compile := exec.CommandContext(ctx, zig, "cc", "--target=wasm32-wasi", "-O2", "-s", "-mexec-model=reactor",
		"-D_GNU_SOURCE", "-I"+engine, "-Wl,-z,stack-size=2097152",
		"-o", filepath.Join(dir, "quickjs.wasm"),
		filepath.Join(engine, "dtoa.c"), filepath.Join(engine, "libregexp.c"),
		filepath.Join(engine, "libunicode.c"), filepath.Join(engine, "quickjs.c"),
		filepath.Join(runtime, "quickjs", "runtime.c"))
	compile.Stdout, compile.Stderr = &output, &output
	if err := compile.Run(); err != nil {
		return errors.Errorf("zig cc: %v\n%s", err, output.Bytes())
	}
	return nil
}
