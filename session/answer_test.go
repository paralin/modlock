package session

import (
	"context"
	"errors"
	"testing"

	"github.com/paralin/modlock/src/host_app/control"
)

// TestAnswerEndsEachForwardedCall answers the host's forwarded calls with the
// service's reply, its error, or the refusal of a session that forwards none.
func TestAnswerEndsEachForwardedCall(t *testing.T) {
	link, err := NewControl()
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	turns := make(chan struct{}, maxAnswers)

	// The service sees the call as the host sent it.
	call := &control.ServiceCall{Id: 7, Mod: "arena", Service: "Drills", Method: "list", Payload: []byte("{}")}
	reply := func(_ context.Context, got *control.ServiceCall) ([]byte, error) {
		if got.GetMod() != "arena" || got.GetMethod() != "list" || string(got.GetPayload()) != "{}" {
			t.Errorf("the service got %v", got)
		}
		return []byte("[]"), nil
	}
	failed := func(context.Context, *control.ServiceCall) ([]byte, error) { return nil, errors.New("refused") }
	silent := func(context.Context, *control.ServiceCall) ([]byte, error) { return nil, errors.New("") }

	// Each answer carries the call's id and the outcome.
	for _, check := range []struct {
		name           string
		fn             func(context.Context, *control.ServiceCall) ([]byte, error)
		payload, error string
	}{
		{"reply", reply, "[]", ""},
		{"error", failed, "", "refused"},
		{"empty error", silent, "", "the service failed"},
		{"no forwarder", nil, "", "this session forwards no services"},
	} {
		answer(t.Context(), link, check.fn, turns, call)
		ended := (<-link.requests).GetAnswer()
		if ended.GetId() != 7 || string(ended.GetPayload()) != check.payload || ended.GetError() != check.error {
			t.Errorf("%s: answered %v, want payload %q error %q", check.name, ended, check.payload, check.error)
		}
	}
	if len(turns) != 0 {
		t.Errorf("%d turns still held", len(turns))
	}
}
