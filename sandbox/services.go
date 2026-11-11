package sandbox

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"

	"github.com/paralin/modlock/src/wasm"
	"github.com/pkg/errors"
)

// FixtureDir is the directory of a mod project that holds its service
// fixtures: one file per service, <name>.json, an object whose keys are the
// service's methods and whose values are each method's reply, sent as that
// JSON's bytes.
const FixtureDir = "services"

// fixture answers one service from a fixture file.
type fixture struct {
	// path is the fixture file.
	path string
	// replies are the replies by method.
	replies map[string]json.RawMessage
}

// provide finds the provider of each service build's manifest names, logs
// which answers each, and keeps the fixtures on build. It fails, naming the
// service, when a service the mod needs has no provider.
func (s *Sandbox) provide(path string, build *Build) error {
	// Find the mod's fixtures, if its build has a project.
	mod := build.Manifest.GetSlug()
	var dir string
	if s.config.Fixtures != nil {
		dir = s.config.Fixtures(path)
	}

	// Name each service's provider: the sandbox itself, a fixture or nobody.
	var missing error
	for _, service := range build.Manifest.GetServices() {
		name := service.GetName()
		provider, err := s.provider(dir, name)
		switch {
		case err != nil:
			return err
		case provider == nil && name == scoresService:
			s.log(mod, "service "+name+": the sandbox")
		case provider != nil:
			s.log(mod, "service "+name+": fixture "+provider.path)
			if build.fixtures == nil {
				build.fixtures = map[string]*fixture{}
			}
			build.fixtures[name] = provider
		case service.GetOptional():
			s.log(mod, "service "+name+": nobody (optional)")
		default:
			s.log(mod, "service "+name+": nobody")
			if missing == nil {
				missing = errors.Errorf("the mod needs service %s, which nobody provides here; add %s", name, filepath.Join(FixtureDir, name+".json"))
			}
		}
	}
	return missing
}

// provider reads the fixture of service name in dir, or is nil when dir holds
// none.
func (s *Sandbox) provider(dir, name string) (*fixture, error) {
	// A service without a fixture file has no fixture; a name that is not one
	// file name never reads outside dir.
	if dir == "" || name == "" || name == "." || name == ".." || filepath.Base(name) != name {
		return nil, nil
	}
	path := filepath.Join(dir, name+".json")
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, errors.Wrap(err, "read the fixture of service "+name)
	}

	// Read its replies by method.
	replies := map[string]json.RawMessage{}
	if err := json.Unmarshal(data, &replies); err != nil {
		return nil, errors.Wrapf(err, "parse %s, an object of replies by method", path)
	}
	return &fixture{path: path, replies: replies}, nil
}

// answer replies to call from the fixture, or fails for a method it lacks.
func (f *fixture) answer(call *wasm.ServiceCall) *wasm.Reply {
	// Find the method's reply.
	reply, ok := f.replies[call.GetMethod()]
	if !ok {
		return &wasm.Reply{Error: "the fixture " + f.path + " has no method " + call.GetMethod()}
	}

	// Send it as compact JSON.
	var compact bytes.Buffer
	if err := json.Compact(&compact, reply); err != nil {
		return &wasm.Reply{Error: err.Error()}
	}
	response, _ := (&wasm.ServiceReply{Payload: compact.Bytes()}).MarshalVT()
	return &wasm.Reply{Response: response}
}
