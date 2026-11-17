package main

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func TestProfileWritesAFlameGraph(t *testing.T) {
	// Time the profiled mod, which spins 2 ms a frame in its OnFrame, as
	// modlock profile does.
	dist := buildProfiled(t)
	prof := newProfiler(&printer{}, []string{dist}, true)
	prof.say = func(string) {}
	taken := timeInSandbox(context.Background(), prof, dist, nil, 3*time.Second)
	if len(taken) != 1 {
		t.Fatalf("took %d mods' timings, want 1", len(taken))
	}
	data, err := speedscopeFile(taken)
	if err != nil {
		t.Fatal(err)
	}

	// The file is a speedscope file whose one profile stacks each handler on
	// the mod, weighted by the milliseconds it spent.
	var file struct {
		Schema string `json:"$schema"`
		Shared struct {
			Frames []speedscopeFrame `json:"frames"`
		} `json:"shared"`
		Profiles []speedscopeProfile `json:"profiles"`
	}
	if err := json.Unmarshal(data, &file); err != nil {
		t.Fatal(err)
	}
	if file.Schema != speedscopeSchema || len(file.Profiles) != 1 {
		t.Fatalf("file:\n%s", data)
	}
	profile := file.Profiles[0]
	if profile.Type != "sampled" || profile.Unit != "milliseconds" || profile.Name != "profiled" ||
		len(profile.Samples) != len(profile.Weights) || len(profile.Samples) < 2 {
		t.Fatalf("profile:\n%s", data)
	}
	total, onFrame := 0.0, 0.0
	for i, stack := range profile.Samples {
		if len(stack) != 2 || file.Shared.Frames[stack[0]].Name != "profiled" {
			t.Fatalf("sample %d is %v, want the mod then a handler:\n%s", i, stack, data)
		}
		total += profile.Weights[i]
		handler := file.Shared.Frames[stack[1]]
		if strings.HasPrefix(handler.Name, "OnFrame ") {
			if !strings.HasSuffix(handler.File, "testdata/profiled/main.go") || handler.Line != 23 {
				t.Errorf("OnFrame at %s:%d, want testdata/profiled/main.go:23", handler.File, handler.Line)
			}
			onFrame = profile.Weights[i]
		}
	}
	frames := taken[0].GetFrames()
	if onFrame < 2*float64(frames.GetCalls()) {
		t.Errorf("OnFrame weighs %.1f ms over %d frames, want at least 2 ms each:\n%s", onFrame, frames.GetCalls(), data)
	}
	if profile.EndValue != total {
		t.Errorf("the profile ends at %.1f ms, want its samples' %.1f", profile.EndValue, total)
	}
	t.Logf("%s", data)
}
