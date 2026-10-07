// Package js builds the modlock library that TypeScript and JavaScript mods
// import: one ES module holding the calls into the game and its protobuf
// runtime, the entity classes as a second module that imports the first, and
// their declarations. It also bundles the renderer that draws mods'
// interfaces in the player's game.
//
// Library builds from the Modlock source when it is on disk: a checkout, or
// the module cache of the build that links this package. A release build has
// no source on disk and downloads the library built for its release.
package js

import (
	"archive/tar"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"runtime/debug"
	"strings"

	"github.com/evanw/esbuild/pkg/api"
	"github.com/paralin/modlock/internal/fetch"
	"github.com/paralin/modlock/internal/npm"
	"github.com/pkg/errors"
	"golang.org/x/mod/module"
)

// Archive is the release asset that holds the built library.
const Archive = "modlock-library.tar.gz"

// Renderer is the built renderer's file name, which a client content package
// installs as scripts/modlock/ui.js beside the layout in panorama/layout.
const Renderer = "ui.js"

// Files are the built library's files, which modlock build installs into
// each script project.
var Files = []string{"index.js", "index.d.ts", "globals.d.ts", "host.gen.d.ts", "entities.js", "entities.d.ts", "console.js", "console.d.ts", "ui.d.ts", "bits.d.ts", "spot.d.ts", "dropper.d.ts"}

// modulePath is the Modlock module path.
const modulePath = "github.com/paralin/modlock"

// protobufVersion is the protobuf runtime the library bundles.
const protobufVersion = "1.1.1"

// banner is the legal comment that every bundle keeps for the protobuf
// runtime it includes.
const banner = "/*! Includes protobuf-es-lite under the Apache License 2.0: https://www.apache.org/licenses/LICENSE-2.0 */"

// revision changes whenever Build changes its output, so a cached build from
// the same sources is rebuilt.
const revision = "4"

// sources are the paths under the Modlock source that Build reads.
var sources = []string{"js/src", "js/tsconfig.json", "proto/modlock", "panorama/src"}

// Library returns a directory holding the built library and renderer,
// building or downloading it into the user cache the first time.
func Library(ctx context.Context) (string, error) {
	if source := Source(); source != "" {
		return buildCached(ctx, source)
	}
	version := moduleVersion()
	if version == "" || module.IsPseudoVersion(version) || strings.HasSuffix(version, "+dirty") {
		return "", errors.New("this modlock build has neither the Modlock source nor a release; build it from a Modlock checkout")
	}
	dir, err := fetch.Cache("library", version)
	if err != nil {
		return "", err
	}
	err = fetch.Install(dir, func(staging string) error {
		archive := dir + ".tar.gz"
		defer os.Remove(archive)
		if err := fetch.File(ctx, fetch.Release(version, Archive), archive); err != nil {
			return err
		}
		return unpack(archive, staging)
	})
	if err != nil {
		return "", errors.Wrap(err, "download the modlock library")
	}
	return dir, nil
}

// Source returns the root of the Modlock source on disk, or empty when this
// build has none: the checkout or module cache directory this package was
// compiled from, or else the module cache entry of the linked version.
func Source() string {
	var candidates []string
	if _, file, _, ok := runtime.Caller(0); ok && filepath.IsAbs(file) {
		candidates = append(candidates, filepath.Dir(filepath.Dir(file)))
	}
	if version := moduleVersion(); version != "" {
		candidates = append(candidates, filepath.Join(moduleCache(), filepath.FromSlash(modulePath)+"@"+version))
	}
	for _, dir := range candidates {
		if _, err := os.Stat(filepath.Join(dir, "js", "src", "index.ts")); err == nil {
			return dir
		}
	}
	return ""
}

// moduleVersion returns the version of Modlock linked into this build, or
// empty for a build of a checkout.
func moduleVersion() string {
	info, ok := debug.ReadBuildInfo()
	if !ok {
		return ""
	}
	version := ""
	if info.Main.Path == modulePath {
		version = info.Main.Version
	}
	for _, dep := range info.Deps {
		if dep.Path == modulePath && dep.Replace == nil {
			version = dep.Version
		}
	}
	if version == "(devel)" {
		return ""
	}
	return version
}

// moduleCache returns the Go module cache directory.
func moduleCache() string {
	if dir := os.Getenv("GOMODCACHE"); dir != "" {
		return dir
	}
	gopath, _, _ := strings.Cut(os.Getenv("GOPATH"), string(filepath.ListSeparator))
	if gopath == "" {
		home, _ := os.UserHomeDir()
		gopath = filepath.Join(home, "go")
	}
	return filepath.Join(gopath, "pkg", "mod")
}

// buildCached returns the library built from source, keyed by the sources
// and this build's revision, building it the first time.
func buildCached(ctx context.Context, source string) (string, error) {
	sum := sha256.New()
	_, _ = io.WriteString(sum, revision+"\x00"+protobufVersion+"\x00"+npm.TsgoVersion+"\x00")
	err := walkSources(source, func(name string, data []byte) error {
		_, _ = io.WriteString(sum, name+"\x00")
		_, _ = sum.Write(data)
		return nil
	})
	if err != nil {
		return "", err
	}
	dir, err := fetch.Cache("library", "src-"+hex.EncodeToString(sum.Sum(nil))[:16])
	if err != nil {
		return "", err
	}
	if err := fetch.Install(dir, func(staging string) error { return Build(ctx, source, staging) }); err != nil {
		return "", errors.Wrap(err, "build the modlock library")
	}
	return dir, nil
}

// walkSources calls visit with each source file Build reads, by its slash
// path under source, in a stable order.
func walkSources(source string, visit func(name string, data []byte) error) error {
	root := os.DirFS(source)
	for _, top := range sources {
		err := fs.WalkDir(root, top, func(name string, entry fs.DirEntry, err error) error {
			if err != nil || entry.IsDir() {
				return err
			}
			if strings.HasSuffix(name, ".test.ts") || (strings.HasPrefix(name, "proto/") && !strings.HasSuffix(name, ".pb.ts")) {
				return nil
			}
			data, err := fs.ReadFile(root, name)
			if err != nil {
				return err
			}
			return visit(name, data)
		})
		if err != nil {
			return err
		}
	}
	return nil
}

// Build builds the library and renderer from the Modlock source into out.
func Build(ctx context.Context, source, out string) error {
	// Lay the sources out beside the protobuf runtime they import.
	scratch, err := os.MkdirTemp("", "modlock-library-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(scratch)
	err = walkSources(source, func(name string, data []byte) error {
		return write(filepath.Join(scratch, filepath.FromSlash(name)), data)
	})
	if err != nil {
		return err
	}
	protobuf, err := npm.Package(ctx, "@aptre/protobuf-es-lite", protobufVersion)
	if err != nil {
		return err
	}
	if err := copyTree(protobuf, filepath.Join(scratch, "node_modules", "@aptre", "protobuf-es-lite")); err != nil {
		return err
	}

	// Declare the library.
	tsgo, err := npm.Tsgo(ctx)
	if err != nil {
		return err
	}
	types := filepath.Join(scratch, "types")
	declare := exec.CommandContext(ctx, tsgo, "--project", filepath.Join(scratch, "js", "tsconfig.json"), "--outDir", types)
	if output, err := declare.CombinedOutput(); err != nil {
		return errors.Errorf("declare the library: %s", output)
	}
	declared := filepath.Join(types, "js", "src")
	globals, err := os.ReadFile(filepath.Join(scratch, "js", "src", "globals.d.ts"))
	if err != nil {
		return err
	}
	index, err := os.ReadFile(filepath.Join(declared, "index.d.ts"))
	if err != nil {
		return err
	}
	files := map[string][]byte{
		"globals.d.ts": globals,
		"index.d.ts":   append([]byte("/// <reference path=\"./globals.d.ts\" />\n"), index...),
	}
	for _, name := range []string{"host.gen.d.ts", "entities.d.ts", "console.d.ts", "ui.d.ts", "bits.d.ts", "spot.d.ts", "dropper.d.ts"} {
		if files[name], err = os.ReadFile(filepath.Join(declared, name)); err != nil {
			return err
		}
	}

	// Bundle the library, the entity classes, the console and the renderer.
	bundles := []struct {
		name    string
		options api.BuildOptions
	}{
		{"index.js", api.BuildOptions{EntryPoints: []string{"js/src/index.ts"}, Format: api.FormatESModule, Target: api.ES2023}},
		{"entities.js", api.BuildOptions{EntryPoints: []string{"js/src/entities.ts"}, Format: api.FormatESModule, Target: api.ES2023, External: []string{"modlock"}}},
		{"console.js", api.BuildOptions{EntryPoints: []string{"js/src/console.ts"}, Format: api.FormatESModule, Target: api.ES2023, External: []string{"modlock"}}},
		{Renderer, api.BuildOptions{EntryPoints: []string{"panorama/src/main.ts"}, Format: api.FormatIIFE, Target: api.ES2020, MinifyWhitespace: true, MinifyIdentifiers: true, MinifySyntax: true}},
	}
	for _, bundle := range bundles {
		options := bundle.options
		options.AbsWorkingDir = scratch
		options.Bundle = true
		options.Platform = api.PlatformBrowser
		options.Banner = map[string]string{"js": banner}
		options.LogLevel = api.LogLevelSilent
		result := api.Build(options)
		if len(result.Errors) != 0 {
			messages := api.FormatMessages(result.Errors, api.FormatMessagesOptions{Kind: api.ErrorMessage})
			return errors.Errorf("bundle %s: %s", bundle.name, strings.Join(messages, ""))
		}
		files[bundle.name] = result.OutputFiles[0].Contents
	}
	for name, data := range files {
		if err := write(filepath.Join(out, name), data); err != nil {
			return err
		}
	}
	return nil
}

// Pack writes the library and renderer built from source as the gzipped
// tar archive a release publishes.
func Pack(ctx context.Context, source, archive string) error {
	built, err := os.MkdirTemp("", "modlock-library-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(built)
	if err := Build(ctx, source, built); err != nil {
		return err
	}
	file, err := os.Create(archive)
	if err != nil {
		return err
	}
	defer file.Close()
	compressed := gzip.NewWriter(file)
	writer := tar.NewWriter(compressed)
	for _, name := range append(Files, Renderer) {
		data, err := os.ReadFile(filepath.Join(built, name))
		if err != nil {
			return err
		}
		if err := writer.WriteHeader(&tar.Header{Name: name, Mode: 0o644, Size: int64(len(data))}); err != nil {
			return err
		}
		if _, err := writer.Write(data); err != nil {
			return err
		}
	}
	if err := writer.Close(); err != nil {
		return err
	}
	if err := compressed.Close(); err != nil {
		return err
	}
	return file.Close()
}

// unpack writes the files of the library archive at archive into dir.
func unpack(archive, dir string) error {
	return fetch.WalkTar(archive, func(name string, _ fs.FileMode, body io.Reader) error {
		if strings.Contains(name, "/") || !filepath.IsLocal(name) {
			return nil
		}
		data, err := io.ReadAll(body)
		if err != nil {
			return err
		}
		return write(filepath.Join(dir, name), data)
	})
}

// copyTree copies the files under from into to.
func copyTree(from, to string) error {
	return filepath.WalkDir(from, func(name string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() {
			return err
		}
		relative, err := filepath.Rel(from, name)
		if err != nil {
			return err
		}
		data, err := os.ReadFile(name)
		if err != nil {
			return err
		}
		return write(filepath.Join(to, relative), data)
	})
}

// write writes data to a new file at name, creating its directory.
func write(name string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(name), 0o755); err != nil {
		return err
	}
	return os.WriteFile(name, data, 0o644)
}
