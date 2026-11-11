package sandbox

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"

	"github.com/paralin/modlock/src/host_app/control"
	"github.com/paralin/modlock/src/wasm"
)

// TestServiceFixture answers a service the manifest names from the fixture
// in the mod's project, reply by method, and fails a method the fixture lacks
// and a service the manifest does not name.
func TestServiceFixture(t *testing.T) {
	// Write one fixture and name its service.
	dir := t.TempDir()
	fixture := `{"read": {"key": "slide", "steps": [1, 2]}, "save": true}`
	if err := os.WriteFile(filepath.Join(dir, "example.drills.v1.json"), []byte(fixture), 0o644); err != nil {
		t.Fatal(err)
	}
	var logged []string
	s := &Sandbox{config: Config{
		Fixtures: func(string) string { return dir },
		Events:   func(event *control.HostEvent) { logged = append(logged, event.GetLog().GetText()) },
	}}
	build := &Build{Manifest: &wasm.Manifest{Slug: "trial", Services: []*wasm.Service{{Name: "example.drills.v1"}}}}
	if err := s.provide("build", build); err != nil {
		t.Fatal(err)
	}
	if want := "service example.drills.v1: fixture " + filepath.Join(dir, "example.drills.v1.json"); !slices.Equal(logged, []string{want}) {
		t.Fatalf("logged %q, want %q", logged, want)
	}
	serve := func(service, method string) *wasm.Reply {
		request, _ := (&wasm.ServiceCall{Service: service, Method: method, Payload: []byte(`{"key":"slide"}`)}).MarshalVT()
		return s.answer(build, &wasm.Call{Method: "CallService", Request: request})
	}

	// Answer each method with its reply as compact JSON.
	for method, want := range map[string]string{"read": `{"key":"slide","steps":[1,2]}`, "save": `true`} {
		reply := serve("example.drills.v1", method)
		answer := &wasm.ServiceReply{}
		if reply.GetError() != "" || answer.UnmarshalVT(reply.GetResponse()) != nil || string(answer.GetPayload()) != want {
			t.Fatalf("%s answered %q (error %q), want %s", method, answer.GetPayload(), reply.GetError(), want)
		}
	}

	// Fail a method the fixture lacks, and a service the manifest does not name.
	if reply := serve("example.drills.v1", "list"); !strings.Contains(reply.GetError(), "has no method list") {
		t.Fatalf("a missing method answered %v", reply)
	}
	if reply, want := serve("example.other.v1", "read"), "this host provides no service example.other.v1"; reply.GetError() != want {
		t.Fatalf("error %q, want %q", reply.GetError(), want)
	}
}

// TestServiceProviders starts mods in a project's build that name services:
// the sandbox names each service's provider before the mod starts, starts a
// mod whose optional service nobody provides, and refuses to start one whose
// needed service nobody provides, naming it.
func TestServiceProviders(t *testing.T) {
	// Build the mod once.
	module := filepath.Join(t.TempDir(), "mod.wasm")
	cmd := exec.Command("go", "build", "-buildmode=c-shared", "-o", module, "../check/testdata/hello")
	cmd.Env = append(os.Environ(), "GOOS=wasip1", "GOARCH=wasm")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build the mod: %v\n%s", err, out)
	}
	wasmBytes, err := os.ReadFile(module)
	if err != nil {
		t.Fatal(err)
	}

	// Each case's mod names services, one of which has a fixture.
	cases := []struct {
		name     string
		services string
		want     []string
	}{{
		name:     "optional missing",
		services: `[{"name": "example.drills.v1"}, {"name": "example.ghosts.v1", "optional": true}]`,
		want: []string{
			modLog("service example.drills.v1: fixture %s"),
			modLog("service example.ghosts.v1: nobody (optional)"),
			(&control.HostEvent{Body: &control.HostEvent_Started{Started: &control.ModStarted{Mod: "hello"}}}).String(),
		},
	}, {
		name:     "needed missing",
		services: `[{"name": "example.drills.v1"}, {"name": "example.ghosts.v1"}]`,
		want: []string{
			modLog("service example.drills.v1: fixture %s"),
			modLog("service example.ghosts.v1: nobody"),
			(&control.HostEvent{Body: &control.HostEvent_Failed{Failed: &control.ModFailed{
				Mod:   "hello",
				Error: "the mod needs service example.ghosts.v1, which nobody provides here; add services/example.ghosts.v1.json",
			}}}).String(),
		},
	}}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			// Lay out a project with its build and one fixture.
			project := t.TempDir()
			build := filepath.Join(project, "build")
			fixtures := filepath.Join(project, FixtureDir)
			for _, dir := range []string{build, fixtures} {
				if err := os.MkdirAll(dir, 0o755); err != nil {
					t.Fatal(err)
				}
			}
			manifest := `{"slug": "hello", "version": "0.1.0", "runtime": "RUNTIME_WASM", "entry": "mod.wasm", "services": ` + c.services + `}`
			fixture := filepath.Join(fixtures, "example.drills.v1.json")
			for path, data := range map[string]string{
				filepath.Join(build, "mod.json"): manifest,
				filepath.Join(build, "mod.wasm"): string(wasmBytes),
				fixture:                          `{"read": null}`,
			} {
				if err := os.WriteFile(path, []byte(data), 0o644); err != nil {
					t.Fatal(err)
				}
			}

			// Run the session to its end, keeping the mod's events.
			var mu sync.Mutex
			var events []string
			joined := make(chan struct{})
			box := Start(context.Background(), Config{
				Mods: []string{build},
				Fixtures: func(path string) string {
					if path != build {
						t.Errorf("fixtures asked for %s, want %s", path, build)
					}
					return fixtures
				},
				Events: func(event *control.HostEvent) {
					mu.Lock()
					defer mu.Unlock()
					switch {
					case event.GetJoined() != nil:
						close(joined)
					case event.GetReady() == nil:
						events = append(events, event.String())
					}
				},
			})
			<-joined
			box.Stop()
			if err := box.Wait(); err != nil {
				t.Fatal(err)
			}

			// The providers come first, then the start or the refusal.
			want := slices.Clone(c.want)
			for i := range want {
				want[i] = strings.ReplaceAll(want[i], "%s", fixture)
			}
			if !slices.Equal(events, want) {
				t.Fatalf("got events\n%q\nwant\n%q", events, want)
			}
		})
	}
}

// modLog is the host event of one sandbox line about the hello mod.
func modLog(text string) string {
	return (&control.HostEvent{Body: &control.HostEvent_Log{Log: &control.ModLog{Mod: "hello", Text: text}}}).String()
}
