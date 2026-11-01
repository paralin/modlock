package main

import (
	"os"
	"path/filepath"

	"github.com/paralin/modlock/project"
	"github.com/paralin/modlock/sourcemap"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm/profile"
)

// locateFrames returns a handler that passes each host event to events, first
// naming the source file and line of each script frame in a slow frame
// report, through the source map beside the bundle in that mod's build
// directory among mods. A frame stays as the bundle named it when the mod has
// no map, such as a release build, or the map does not cover its line.
func locateFrames(mods []string, events func(*control.HostEvent)) func(*control.HostEvent) {
	// The host names each mod by its manifest's slug.
	dirs := make(map[string]string, len(mods))
	for _, dir := range mods {
		if manifest, err := project.ReadManifest(dir); err == nil {
			dirs[manifest.GetSlug()] = dir
		}
	}
	return func(event *control.HostEvent) {
		if slow := event.GetSlowFrame(); slow != nil {
			if dir, ok := dirs[slow.GetMod()]; ok {
				locateSamples(dir, slow.GetReport().GetSamples())
			}
		}
		events(event)
	}
}

// locateSamples rewrites the script frames of samples to the sources the map
// in the build directory dir names. It reads the map on each report, about
// once a minute, so a rebuilt mod's frames use the rebuilt map.
func locateSamples(dir string, samples *profile.StackSamples) {
	data, err := os.ReadFile(filepath.Join(dir, project.SourceMapEntry))
	if err != nil {
		return
	}
	sources, err := sourcemap.Parse(data)
	if err != nil {
		return
	}
	for _, stack := range samples.GetStacks() {
		for _, frame := range stack.GetFrames() {
			if frame.GetFile() != project.ScriptEntry {
				continue
			}
			at, ok := sources.Locate(int(frame.GetLine()), int(frame.GetColumn()))
			if !ok {
				continue
			}
			file := filepath.FromSlash(at.Source)
			if !filepath.IsAbs(file) {
				file = filepath.Join(dir, file)
			}
			frame.File, frame.Line, frame.Column = file, uint32(at.Line), uint32(at.Column)
		}
	}
}
