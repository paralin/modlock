package peernet

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/aperturerobotics/go-websocket"
	"github.com/paralin/modlock/publish"
	"github.com/paralin/modlock/src/host_app/control"
	"github.com/s4wave/spacewave/net/crypto"
)

// TestLobbySetupReconnect exchanges a real lobby socket's configuration and
// acknowledgement, then repeats the acknowledgement after a dropped socket.
func TestLobbySetupReconnect(t *testing.T) {
	// The store supplies an opaque setup and checks what survives the network.
	ctx, cancel := context.WithTimeout(t.Context(), 5*time.Second)
	defer cancel()
	reply := &control.ServiceReply{Mod: "arena", Service: "lobby", Method: "setup", Id: "apply-1", Payload: []byte("two players per team")}
	received := make(chan *control.ServiceResult, 2)
	failures := make(chan error, 2)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := websocket.Accept(w, r, nil)
		if err != nil {
			failures <- err
			return
		}
		defer conn.CloseNow()
		_, data, err := conn.Read(ctx)
		if err != nil {
			failures <- err
			return
		}
		open := &publish.DevLobbyOpen{}
		if err := open.UnmarshalVT(data); err != nil {
			failures <- err
			return
		}
		if string(open.GetManifestJson()) != "local manifest" {
			t.Error("the local manifest was lost")
		}
		data, err = (&publish.DevLobbyState{State: &publish.DevLobbyState_Url{Url: "/play?lobby=local"}, Setup: reply}).MarshalVT()
		if err == nil {
			err = conn.Write(ctx, websocket.MessageBinary, data)
		}
		if err != nil {
			failures <- err
			return
		}
		_, data, err = conn.Read(ctx)
		if err != nil {
			failures <- err
			return
		}
		result := &control.ServiceResult{}
		if err := result.UnmarshalVT(data); err != nil {
			failures <- err
			return
		}
		received <- result
	}))
	defer server.Close()

	// The first connection reports adoption to the store. Reconnecting repeats
	// that result without needing another reload of the running mod.
	key, _, err := crypto.GenerateEd25519Key(nil)
	if err != nil {
		t.Fatal(err)
	}
	service := &Service{origin: server.URL, key: key}
	results := make(chan *control.ServiceResult, 1)
	var last *control.ServiceResult
	for attempt := range 2 {
		err := service.holdLobbySocket(ctx, "signed-in", &publish.DevLobbyOpen{ModName: "Arena", Secret: 7, ManifestJson: []byte("local manifest")}, nil, results, &last, func(state *publish.DevLobbyState) {
			if !state.GetSetup().EqualVT(reply) {
				t.Error("the opaque setup changed", state)
			}
			if attempt == 0 {
				results <- &control.ServiceResult{Id: reply.GetId()}
			}
		})
		if err == nil {
			t.Fatal("the dropped socket remained open")
		}
		select {
		case result := <-received:
			if result.GetId() != reply.GetId() || result.GetError() != "" {
				t.Fatal("lost adoption result", result)
			}
		case err := <-failures:
			t.Fatal(err)
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		}
	}
}
