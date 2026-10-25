// Package js builds the modlock library that TypeScript and JavaScript mods
// import: one ES module holding the calls into the game and its protobuf
// runtime, the entity classes as a second module that imports the first, and
// their declarations. Beside them it keeps the library's TypeScript source
// and the protobuf runtime's, which scriptc compiles with a mod. It also
// bundles the renderer that draws mods' interfaces in the player's game, the
// preview page that draws them in a browser, and Modlock Studio's page.
//
// A script project installs only the declarations, so editors and the type
// checker see one modlock package and neither compiler mistakes it for an
// npm dependency; each compiler resolves modlock to its own copy here.
//
// Library builds from the Modlock source when it is on disk: a checkout, or
// the module cache of the build that links this package. A release build has
// no source on disk and carries the library it was built with.
package js

import (
	"context"
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"runtime"
	"runtime/debug"
	"strings"

	"github.com/evanw/esbuild/pkg/api"
	"github.com/paralin/modlock/internal/fetch"
	"github.com/paralin/modlock/internal/npm"
	"github.com/pkg/errors"
)

// Built is the directory under js that the release build fills with the
// built library before it builds the command line, which embeds it.
const Built = "built"

// built holds the library a release build embeds; in any other build it holds
// only the directory's .gitignore.
//
//go:embed all:built
var built embed.FS

// Sources holds the library modules that import nothing, so a Go module that
// vendors Modlock keeps them and its browser pages can import them beside its
// own TypeScript: the HUD kit's tokens, the hero table and the HUD text
// helpers, which a page drawn in the game's look shares with its mods.
//
//go:embed src/theme.ts src/heroes.ts src/format.ts
var Sources embed.FS

// Renderer is the built renderer's file name, which a client content package
// installs as scripts/modlock/ui.js beside the layout in panorama/layout. It
// is not ui.js, since the library's declarations include ui.d.ts: a mod that
// imported modlock/ui would type check and then run the renderer on the
// server.
const Renderer = "renderer.js"

// LayoutLibrary is the built library a mod's layout script includes to read
// its data and routes, which a client content package installs as
// scripts/modlock/layout.js beside the renderer. Its bundle defines the global
// Modlock.
const LayoutLibrary = "layout.js"

// Preview is the directory of the built library that holds the preview page
// modlock dev serves.
const Preview = "preview"

// Studio is the directory of the built library that holds Modlock Studio's
// page, which modlock studio serves.
const Studio = "studio"

// studioPackages are the npm packages Studio's page bundles, by name, at the
// versions the repository's package.json pins for type checking.
var studioPackages = map[string]string{
	"react":     "19.2.0",
	"react-dom": "19.2.0",
	"scheduler": "0.27.0",
	"three":     "0.186.1",
}

// jsxRuntime and jsxTypes are the library's JSX runtime, which the compiler
// and the bundler import as modlock/jsx-runtime.
const (
	jsxRuntime = "export { Fragment, jsx, jsxs } from './index.js'\n"
	jsxTypes   = "export { Fragment, JSX, jsx, jsxs } from './index.js'\n"
)

// Scriptc is the directory of the built library that holds the source scriptc
// compiles: the library under js/src and the generated messages it imports,
// as in the Modlock source, and the protobuf runtime under protobuf-es-lite.
const Scriptc = "scriptc"

// ScriptcProtobuf is the protobuf runtime's source directory under Scriptc.
const ScriptcProtobuf = "protobuf-es-lite"

// modulePath is the Modlock module path.
const modulePath = "github.com/paralin/modlock"

// protobufVersion is the protobuf runtime the library bundles.
const protobufVersion = "1.2.4"

// banner is the legal comment that every bundle keeps for the protobuf
// runtime it includes.
const banner = "/*! Includes protobuf-es-lite under the Apache License 2.0: https://www.apache.org/licenses/LICENSE-2.0 */"

// sources are the paths under the Modlock source that Build reads. Under the
// message roots it reads only the generated TypeScript messages.
var sources = append([]string{"js/src", "js/tsconfig.json", "panorama/src", "panorama/browser", "studio/web", "studio/studio.pb.ts"}, messageRoots...)

// messageRoots are the directories whose generated messages the library and
// renderer import.
var messageRoots = []string{"src", "cli", "publish", "mod/spot"}

// isMessage reports whether name is a generated TypeScript message under one
// of the message roots.
func isMessage(name string) bool {
	return strings.HasSuffix(name, ".pb.ts") && underMessageRoot(name)
}

// underMessageRoot reports whether name lies under one of the message roots.
func underMessageRoot(name string) bool {
	for _, root := range messageRoots {
		if strings.HasPrefix(name, root+"/") {
			return true
		}
	}
	return false
}

// pageFiles are the pages' files that Build copies as they are, by their
// paths under the source and the built library.
var pageFiles = map[string]string{
	"panorama/browser/index.html":  Preview + "/index.html",
	"panorama/browser/preview.css": Preview + "/preview.css",
	"panorama/browser/panels.css":  Preview + "/panels.css",
	"studio/web/index.html":        Studio + "/index.html",
	"studio/web/studio.css":        Studio + "/studio.css",
}

// Library returns a directory holding the built library and renderer: one
// built from the Modlock source when it is on disk, or else the one this build
// embeds, placed in the user cache the first time.
func Library(ctx context.Context) (string, error) {
	if source := Source(); source != "" {
		return buildCached(ctx, source)
	}
	if _, err := fs.Stat(built, path.Join(Built, Renderer)); err == nil {
		return installEmbedded()
	}
	return "", errors.New("this modlock build has neither a built-in library nor the Modlock source; download a Modlock release or build it from a Modlock checkout")
}

// installEmbedded copies the embedded library into the user cache, keyed by
// its contents, the first time.
func installEmbedded() (string, error) {
	// Name the cache directory by the embedded files.
	library, err := fs.Sub(built, Built)
	if err != nil {
		return "", err
	}
	files := map[string][]byte{}
	var names []string
	err = fs.WalkDir(library, ".", func(name string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() || name == ".gitignore" {
			return err
		}
		data, err := fs.ReadFile(library, name)
		if err != nil {
			return err
		}
		files[name] = data
		names = append(names, name)
		return nil
	})
	if err != nil {
		return "", errors.Wrap(err, "read the built-in modlock library")
	}
	sum := sha256.New()
	for _, name := range names {
		_, _ = io.WriteString(sum, name+"\x00")
		_, _ = sum.Write(files[name])
	}
	dir, err := fetch.Cache("library", "built-"+hex.EncodeToString(sum.Sum(nil))[:16])
	if err != nil {
		return "", err
	}

	// Copy the files into place whole.
	err = fetch.Install(dir, func(staging string) error {
		for _, name := range names {
			if err := write(filepath.Join(staging, filepath.FromSlash(name)), files[name]); err != nil {
				return err
			}
		}
		return nil
	})
	if err != nil {
		return "", errors.Wrap(err, "install the built-in modlock library")
	}
	return dir, nil
}

// Source returns the root of the Modlock source on disk, or empty when this
// build has none: the checkout or module cache directory this package was
// compiled from, or else the module cache entry of the linked version.
func Source() string {
	// Gather where the source could be, the compiled checkout first.
	var candidates []string
	if _, file, _, ok := runtime.Caller(0); ok && filepath.IsAbs(file) {
		candidates = append(candidates, filepath.Dir(filepath.Dir(file)))
	}
	if version := moduleVersion(); version != "" {
		candidates = append(candidates, filepath.Join(moduleCache(), filepath.FromSlash(modulePath)+"@"+version))
	}

	// Take the first that holds the library's entry point.
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
	// Read the build's module list.
	info, ok := debug.ReadBuildInfo()
	if !ok {
		return ""
	}

	// Find Modlock as the main module or an unreplaced dependency.
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

// builder is this file under the Modlock source. It holds Build, so its text
// keys the cache and a change to the build rebuilds every cached library.
const builder = "js/library.go"

// buildCached returns the library built from source, keyed by the sources
// and the builder, building it the first time.
func buildCached(ctx context.Context, source string) (string, error) {
	// Name the cache directory by the builder, the sources and the build's
	// other inputs.
	code, err := os.ReadFile(filepath.Join(source, filepath.FromSlash(builder)))
	if err != nil {
		return "", err
	}
	sum := sha256.New()
	_, _ = sum.Write(code)
	_, _ = io.WriteString(sum, "\x00"+protobufVersion+"\x00"+npm.TsgoVersion+"\x00")
	err = walkSources(source, func(name string, data []byte) error {
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

	// Build into it the first time.
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
			// Skip directories, tests and everything beside the messages.
			if err != nil || entry.IsDir() {
				return err
			}
			if strings.HasSuffix(name, ".test.ts") || (underMessageRoot(name) && !isMessage(name)) {
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
	for name, version := range studioPackages {
		dir, err := npm.Package(ctx, name, version)
		if err != nil {
			return err
		}
		if err := copyTree(dir, filepath.Join(scratch, "node_modules", name)); err != nil {
			return err
		}
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

	// Gather the declarations, with index.d.ts referencing the globals.
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
		"globals.d.ts":     globals,
		"index.d.ts":       append([]byte("/// <reference path=\"./globals.d.ts\" />\n"), index...),
		"jsx-runtime.js":   []byte(jsxRuntime),
		"jsx-runtime.d.ts": []byte(jsxTypes),
	}
	emitted, err := os.ReadDir(declared)
	if err != nil {
		return err
	}
	for _, entry := range emitted {
		name := entry.Name()
		if _, written := files[name]; written || !strings.HasSuffix(name, ".d.ts") {
			continue
		}
		if files[name], err = os.ReadFile(filepath.Join(declared, name)); err != nil {
			return err
		}
	}

	// Bundle the library, the entity classes, the console, the renderer and
	// the pages.
	bundles := []struct {
		name    string
		options api.BuildOptions
	}{
		{"index.js", api.BuildOptions{EntryPoints: []string{"js/src/quickjs.ts"}, Format: api.FormatESModule, Target: api.ES2023}},
		{"entities.js", api.BuildOptions{EntryPoints: []string{"js/src/entities.ts"}, Format: api.FormatESModule, Target: api.ES2023, External: []string{"modlock"}}},
		{"console.js", api.BuildOptions{EntryPoints: []string{"js/src/console.ts"}, Format: api.FormatESModule, Target: api.ES2023, External: []string{"modlock"}}},
		{Renderer, api.BuildOptions{EntryPoints: []string{"panorama/src/main.ts"}, Format: api.FormatIIFE, Target: api.ES2020, MinifyWhitespace: true, MinifyIdentifiers: true, MinifySyntax: true}},
		{LayoutLibrary, api.BuildOptions{EntryPoints: []string{"panorama/src/layout.ts"}, Format: api.FormatIIFE, GlobalName: "Modlock", Target: api.ES2020, MinifySyntax: true}},
		{Preview + "/preview.js", api.BuildOptions{EntryPoints: []string{"panorama/browser/preview.ts"}, Format: api.FormatIIFE, Target: api.ES2022, MinifySyntax: true}},
		{Studio + "/studio.js", api.BuildOptions{
			EntryPoints: []string{"studio/web/main.tsx"}, Format: api.FormatIIFE, Target: api.ES2022,
			JSX: api.JSXAutomatic, Define: map[string]string{"process.env.NODE_ENV": `"production"`},
			MinifyWhitespace: true, MinifyIdentifiers: true, MinifySyntax: true,
		}},
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
	for from, to := range pageFiles {
		if files[to], err = os.ReadFile(filepath.Join(scratch, filepath.FromSlash(from))); err != nil {
			return err
		}
	}
	for name, data := range files {
		if err := write(filepath.Join(out, filepath.FromSlash(name)), data); err != nil {
			return err
		}
	}

	// Keep the library's source and the protobuf runtime's for scriptc.
	err = walkSources(source, func(name string, data []byte) error {
		if !strings.HasPrefix(name, "js/src/") && !isMessage(name) {
			return nil
		}
		return write(filepath.Join(out, Scriptc, filepath.FromSlash(name)), data)
	})
	if err != nil {
		return err
	}
	return copyTree(filepath.Join(protobuf, "src"), filepath.Join(out, Scriptc, ScriptcProtobuf))
}

// copyTree copies the files under from into to.
func copyTree(from, to string) error {
	return filepath.WalkDir(from, func(name string, entry fs.DirEntry, err error) error {
		// Copy each file to the same relative path under to.
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
