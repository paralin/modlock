package sandbox

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"

	"github.com/paralin/modlock/persist"
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

	// Keep the records of the persist service in the mod's directory, or for
	// the session when it has none.
	if len(build.Manifest.GetPersist()) != 0 {
		dir := ""
		if s.config.Persist != nil {
			dir = s.config.Persist(path, mod)
		}
		if dir == "" {
			temp, err := os.MkdirTemp("", "modlock-persist-")
			if err != nil {
				return err
			}
			dir = temp
		}
		records, err := persist.Open(dir)
		if err != nil {
			return err
		}
		build.records = records
		s.log(mod, "service "+persist.Service+": the sandbox, kept in "+dir)
	}

	// Name each service's provider: the sandbox itself, a fixture or nobody.
	var missing error
	for _, service := range build.Manifest.GetServices() {
		name := service.GetName()
		provider, err := readFixture(dir, name)
		switch {
		case err != nil:
			return err
		case provider == nil && name == scoresService:
			s.log(mod, "service "+name+": the sandbox")
		case provider == nil && name == persist.Service && build.records != nil:
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

// ReadFixture reads the fixture of service name in dir: its file and each
// method's reply as compact JSON. The path is empty when dir holds none, so a
// game host can answer from the same files the sandbox does.
func ReadFixture(dir, name string) (string, map[string][]byte, error) {
	// Read the file, if there is one.
	found, err := readFixture(dir, name)
	if err != nil || found == nil {
		return "", nil, err
	}

	// Compact each reply as the sandbox sends it.
	replies := make(map[string][]byte, len(found.replies))
	for method, reply := range found.replies {
		var compact bytes.Buffer
		if err := json.Compact(&compact, reply); err != nil {
			return "", nil, errors.Wrapf(err, "compact %s's reply to %s", found.path, method)
		}
		replies[method] = compact.Bytes()
	}
	return found.path, replies, nil
}

// readFixture reads the fixture of service name in dir, or is nil when dir
// holds none.
func readFixture(dir, name string) (*fixture, error) {
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

// persist answers the persist service from build's records, for the
// sandbox's one player in slot 0. A refused call is logged, so the mod's
// author sees why.
func (s *Sandbox) persist(build *Build, call *wasm.ServiceCall) *wasm.Reply {
	players := func(slot int32) (uint64, bool) {
		return player.GetSteamId(), slot == player.GetPlayer()
	}
	response, err := persist.Answer(build.records, build.Manifest, players, call.GetMethod(), call.GetPayload())
	if err != nil {
		s.log(build.Manifest.GetSlug(), "persist "+call.GetMethod()+": "+err.Error())
		return &wasm.Reply{Error: err.Error()}
	}
	reply, _ := (&wasm.ServiceReply{Payload: response}).MarshalVT()
	return &wasm.Reply{Response: reply}
}

// maxServiceCalls bounds the calls a mod starts between frames, as the host
// does.
const maxServiceCalls = 256

// serviceCalls holds one mod instance's started service calls. Every
// provider here answers at once, so each call ends in the next frame.
type serviceCalls struct {
	// next is the number of the last call started.
	next uint64
	// ended holds the ends the next frame delivers, in call order.
	ended []*wasm.ServiceRepliedEvent
}

// callService answers request from the provider of its service: the
// sandbox's scores or the mod's fixture. It is nil when nobody provides the
// service.
func (s *Sandbox) callService(build *Build, request *wasm.ServiceCall) *wasm.Reply {
	if request.GetService() == scoresService {
		return s.scores(build.Manifest, request)
	}
	if request.GetService() == persist.Service && build.records != nil {
		return s.persist(build, request)
	}
	if provider := build.fixtures[request.GetService()]; provider != nil {
		return provider.answer(request)
	}
	return nil
}

// startServiceCall numbers a StartServiceCall from the mod build describes
// and keeps its end for the next frame. The provider answers through the
// same path as a CallService, so a mod cannot tell a fixture from another
// provider; a service nobody provides is never asked.
func (s *Sandbox) startServiceCall(build *Build, calls *serviceCalls, call *wasm.Call) *wasm.Reply {
	// Decode the call, refusing one past the bound.
	request := &wasm.StartServiceCallRequest{}
	if err := request.UnmarshalVT(call.GetRequest()); err != nil {
		return &wasm.Reply{Error: "the StartServiceCall request is malformed"}
	}
	if len(calls.ended) >= maxServiceCalls {
		return &wasm.Reply{Error: "the mod already holds " + strconv.Itoa(maxServiceCalls) + " service calls"}
	}
	calls.next++
	event := &wasm.ServiceRepliedEvent{Call: calls.next}

	// Ask the provider now, and keep the answer, its refusal or the absence
	// of a provider.
	reply := s.callService(build, &wasm.ServiceCall{
		Service: request.GetService(),
		Method:  request.GetMethod(),
		Payload: request.GetPayload(),
	})
	switch {
	case reply == nil:
		event.Failure = wasm.ServiceFailure_SERVICE_FAILURE_UNAVAILABLE
		event.Error = "this host provides no service " + request.GetService()
	case reply.GetError() != "":
		event.Failure = wasm.ServiceFailure_SERVICE_FAILURE_REFUSED
		event.Error = reply.GetError()
	default:
		answer := &wasm.ServiceReply{}
		_ = answer.UnmarshalVT(reply.GetResponse())
		event.Payload = answer.GetPayload()
	}
	calls.ended = append(calls.ended, event)

	// Return the call's number.
	started, _ := (&wasm.ServiceCallStarted{Call: calls.next}).MarshalVT()
	return &wasm.Reply{Response: started}
}

// endCalls delivers the ends of mod's started service calls.
func (s *Sandbox) endCalls(ctx context.Context, mod *running) {
	if mod.calls == nil || len(mod.calls.ended) == 0 {
		return
	}
	ended := mod.calls.ended
	mod.calls.ended = nil
	for _, event := range ended {
		if !s.deliver(ctx, mod, "ServiceReplied", event, nil) {
			return
		}
	}
}
