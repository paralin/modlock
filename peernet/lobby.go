package peernet

import (
	"context"
	"time"

	"github.com/aperturerobotics/go-websocket"
	"github.com/paralin/modlock/publish"
	"github.com/pkg/errors"
)

// DevLobbyPath is where the service keeps a hosted session's dev lobby open
// for as long as its socket stays open.
const DevLobbyPath = "/api/dev-lobby"

// lobbyAnswerTimeout bounds opening the dev lobby socket and reading the
// service's answer.
const lobbyAnswerTimeout = 30 * time.Second

// holdLobby keeps a dev lobby for the hosted session secret open at the
// service until ctx ends, reporting each change. It opens the socket again
// after it drops, which resumes the lobby, and stops once the service refuses
// or there is no sign-in. Ending ctx closes the socket normally, which ends
// the lobby. The peer's new links use the lobby's relay until it returns.
func (s *Service) holdLobby(ctx context.Context, req *LobbyRequest, secret uint64, report func(*LobbyState)) {
	// Ask for the session's lobby, and drop its relay once it closes.
	open := &publish.DevLobbyOpen{ModName: req.GetModName(), Version: req.GetVersion(), Secret: secret}
	defer s.relay.Hold(nil)
	retry := time.Second
	var last *LobbyState
	for {
		// Hold the lobby with the current sign-in, reporting only changes,
		// since a refresh of the relay repeats the lobby's state.
		token := s.signIn().Token
		if token == "" {
			report(&LobbyState{State: &LobbyState_SignedOut{SignedOut: true}})
			return
		}
		err := s.holdLobbySocket(ctx, token, open, func(state *publish.DevLobbyState) {
			retry = time.Second
			s.relay.Hold(state.GetRelay())
			if next := lobbyState(state); !next.EqualVT(last) {
				last = next
				report(next)
			}
		})
		switch {
		case ctx.Err() != nil:
			return
		case errors.Is(err, errRefused):
			report(&LobbyState{State: &LobbyState_Refused{Refused: "no lobby: " + s.origin + " refused the saved sign-in; modlock login signs in again"}})
			return
		case errors.Is(err, errLobbyRefused):
			return
		}

		// Open it again after a growing wait.
		last = &LobbyState{State: &LobbyState_Reconnecting{Reconnecting: err.Error()}}
		report(last)
		select {
		case <-time.After(retry):
		case <-ctx.Done():
			return
		}
		retry = min(retry*2, admitRetryMax)
	}
}

// errLobbyRefused reports that the service answered with a refusal.
var errLobbyRefused = errors.New("the service refused the lobby")

// holdLobbySocket opens one dev lobby socket with token, passes each of the
// service's states to answered, and holds the socket until it drops or ctx
// ends. It returns errLobbyRefused after a refusal.
func (s *Service) holdLobbySocket(ctx context.Context, token string, open *publish.DevLobbyOpen, answered func(*publish.DevLobbyState)) error {
	// Open the socket.
	dial, cancel := context.WithTimeout(ctx, lobbyAnswerTimeout)
	defer cancel()
	conn, err := dialSigned(dial, s.origin+DevLobbyPath, token, s.key)
	if err != nil {
		return err
	}
	defer conn.CloseNow()

	// Send the request. Reading on a context that ends would drop the socket
	// without a close, which only marks the host gone, so the socket reads
	// until it closes and ctx ending closes it normally.
	answer, cancelAnswer := context.WithTimeout(context.WithoutCancel(ctx), lobbyAnswerTimeout)
	defer cancelAnswer()
	data, err := open.MarshalVT()
	if err == nil {
		err = conn.Write(answer, websocket.MessageBinary, data)
	}
	if err != nil {
		return err
	}

	// Read the answer.
	state, err := readLobbyState(answer, conn)
	if err != nil {
		return err
	}
	answered(state)
	if state.GetRefused() != nil {
		return errLobbyRefused
	}

	// Read the service's later states until the socket closes.
	states := make(chan *publish.DevLobbyState)
	closed, lost := context.WithCancel(context.WithoutCancel(ctx))
	defer lost()
	go func() {
		defer lost()
		for {
			state, err := readLobbyState(closed, conn)
			if err != nil {
				return
			}
			select {
			case states <- state:
			case <-closed.Done():
				return
			}
		}
	}()

	// Ping it and pass on each state until either side closes it.
	ticker := time.NewTicker(admitPing)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return conn.Close(websocket.StatusNormalClosure, "")
		case <-closed.Done():
			return errors.New("the connection to " + s.origin + " was lost")
		case state := <-states:
			answered(state)
		case <-ticker.C:
			if err := conn.Ping(closed); err != nil {
				return err
			}
		}
	}
}

// readLobbyState reads one DevLobbyState from conn.
func readLobbyState(ctx context.Context, conn *websocket.Conn) (*publish.DevLobbyState, error) {
	_, data, err := conn.Read(ctx)
	if err != nil {
		return nil, err
	}
	state := &publish.DevLobbyState{}
	return state, state.UnmarshalVT(data)
}

// lobbyState reports the service's answer as a lobby state.
func lobbyState(state *publish.DevLobbyState) *LobbyState {
	if refused := state.GetRefused(); refused != nil {
		return &LobbyState{State: &LobbyState_Refused{Refused: refused.GetMessage()}}
	}
	return &LobbyState{State: &LobbyState_Url{Url: state.GetUrl()}}
}
