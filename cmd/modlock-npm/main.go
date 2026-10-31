// Command modlock-npm assembles the npm packages of a Modlock release, in the
// layout esbuild uses: one package per system holding that system's command
// line, and @modlock/modlock, which holds the TypeScript library and its
// declarations and a launcher that runs the command line npm installed for
// the system it runs on:
//
//	modlock-npm -version v0.2.0 -dist dist -library js/built -out dist/npm
//
// -dist holds the release's command lines, named modlock-<os>-<arch>, and
// -library the built library. Each package goes in its own directory under
// -out, named after the package without its scope, ready for npm publish;
// publish the system packages before @modlock/modlock, which depends on them.
//
// npm refuses the unscoped name modlock as too close to another package's.
// Mods still import modlock: the command line writes the declarations there.
package main

import (
	_ "embed"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/paralin/modlock/js"
)

// launcher is @modlock/modlock's bin/modlock.js.
//
//go:embed launcher.js
var launcher []byte

// system is one system the release builds the command line for.
type system struct {
	// goos and goarch name it to Go and in the release's file names.
	goos, goarch string
	// os and cpu name it to npm and in the package's name.
	os, cpu string
}

// systems are the systems the release workflow builds.
var systems = []system{
	{"darwin", "arm64", "darwin", "arm64"},
	{"darwin", "amd64", "darwin", "x64"},
	{"linux", "amd64", "linux", "x64"},
	{"linux", "arm64", "linux", "arm64"},
	{"windows", "amd64", "win32", "x64"},
}

// repository is the packages' source repository.
var repository = map[string]string{"type": "git", "url": "git+https://github.com/paralin/modlock.git"}

func main() {
	tag := flag.String("version", "", "release tag, such as v0.2.0")
	dist := flag.String("dist", "dist", "directory holding the release's command lines")
	library := flag.String("library", filepath.Join("js", js.Built), "built library directory")
	out := flag.String("out", filepath.Join("dist", "npm"), "directory to write the packages into")
	flag.Parse()
	version, ok := strings.CutPrefix(*tag, "v")
	if !ok {
		fmt.Fprintln(os.Stderr, "modlock-npm: -version names a release tag, such as v0.2.0")
		os.Exit(2)
	}
	if err := assemble(version, *dist, *library, *out); err != nil {
		fmt.Fprintln(os.Stderr, "modlock-npm:", err)
		os.Exit(1)
	}
}

// assemble writes every package of the release at version to out.
func assemble(version, dist, library, out string) error {
	// Write a package per system, holding its command line.
	dependencies := map[string]string{}
	for _, s := range systems {
		name := "@modlock/" + s.os + "-" + s.cpu
		dependencies[name] = version
		binary := "modlock"
		if s.goos == "windows" {
			binary += ".exe"
		}
		release := "modlock-" + s.goos + "-" + s.goarch + strings.TrimPrefix(binary, "modlock")
		dir := filepath.Join(out, s.os+"-"+s.cpu)
		err := writePackage(dir, map[string]any{
			"name":            name,
			"version":         version,
			"description":     "The modlock command line for " + s.os + " " + s.cpu + ".",
			"license":         "MIT",
			"repository":      repository,
			"os":              []string{s.os},
			"cpu":             []string{s.cpu},
			"preferUnplugged": true,
		}, map[string]string{filepath.Join("bin", binary): filepath.Join(dist, release)})
		if err != nil {
			return err
		}
	}

	// Write @modlock/modlock: the library's modules and declarations, with the
	// launcher. The game's renderer and layout library stay out, since a mod
	// never imports them.
	files := map[string]string{}
	entries, err := os.ReadDir(library)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		name := entry.Name()
		if entry.IsDir() || name == js.Renderer || name == js.LayoutLibrary || !strings.HasSuffix(name, ".js") && !strings.HasSuffix(name, ".d.ts") {
			continue
		}
		files[name] = filepath.Join(library, name)
	}
	if _, ok := files["index.d.ts"]; !ok {
		return fmt.Errorf("%s holds no built library; run go run ./cmd/modlock-library", library)
	}
	files["LICENSE"] = "LICENSE"
	dir := filepath.Join(out, "modlock")
	err = writePackage(dir, map[string]any{
		"name":                 "@modlock/modlock",
		"version":              version,
		"description":          "Build, test and publish mods for Deadlock.",
		"license":              "MIT",
		"repository":           repository,
		"homepage":             "https://github.com/paralin/modlock#readme",
		"type":                 "module",
		"types":                "index.d.ts",
		"sideEffects":          false,
		"bin":                  map[string]string{"modlock": "bin/modlock.js"},
		"optionalDependencies": dependencies,
	}, files)
	if err != nil {
		return err
	}
	readme := "# modlock\n\nBuild, test and publish mods for Deadlock. This package installs the\n`modlock` command line and the TypeScript library mods import:\n\n```sh\nnpm install --save-dev @modlock/modlock\nnpx modlock new my-mod\n```\n\nSee https://github.com/paralin/modlock for the guide.\n"
	if err := os.WriteFile(filepath.Join(dir, "README.md"), []byte(readme), 0o644); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, "bin", "modlock.js"), launcher, 0o755)
}

// writePackage writes a package to dir: its package.json from manifest, and
// each file in files, by its path in the package, copied from the path it
// maps to. A command line stays executable.
func writePackage(dir string, manifest map[string]any, files map[string]string) error {
	if err := os.MkdirAll(filepath.Join(dir, "bin"), 0o755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(dir, "package.json"), append(data, '\n'), 0o644); err != nil {
		return err
	}
	for name, source := range files {
		data, err := os.ReadFile(source)
		if err != nil {
			return err
		}
		mode := os.FileMode(0o644)
		if filepath.Dir(name) == "bin" {
			mode = 0o755
		}
		if err := os.WriteFile(filepath.Join(dir, name), data, mode); err != nil {
			return err
		}
	}
	return nil
}
