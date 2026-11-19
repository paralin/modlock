package mod

import (
	"errors"

	scorespb "github.com/paralin/modlock/src/wasm/scores"
)

// scoresService names the host service that keeps scores.
const scoresService = "scores"

// ScoreConnection is the connection enrolled by BindScorePlayer.
type ScoreConnection = scorespb.ScoreConnection

// ScoreReport reports one enrolled player's result to a board.
type ScoreReport = scorespb.ScoreReport

// MatchReport reports one match's places to a board.
type MatchReport = scorespb.MatchReport

// MatchPlace names an enrolled participant and their finishing place.
type MatchPlace = scorespb.Place

// BoardQuery selects a page of a board and optionally a viewer.
type BoardQuery = scorespb.BoardQuery

// BoardPage is a page of standings and the requested viewer's own row.
type BoardPage = scorespb.BoardPage

// BindScorePlayer enrolls a live connection for reporting after it leaves.
func BindScorePlayer(player *Connection) (*ScoreConnection, error) {
	if player == nil {
		return nil, errors.New("a live player connection is required")
	}

	// Capture the connection at enrollment rather than at finalization.
	data, err := scoreIdentity(player).MarshalVT()
	if err != nil {
		return nil, err
	}
	reply, err := CallService(scoresService, "Bind", data)
	if err != nil {
		return nil, err
	}
	bound := &ScoreConnection{}
	return bound, bound.UnmarshalVT(reply)
}

// ReportScore reports an enrolled player's score. Done receives acceptance or
// refusal on a later frame. Key distinguishes rounds within the game.
func ReportScore(player *ScoreConnection, report *ScoreReport, done func(error)) {
	report = report.CloneVT()
	report.Player, report.Connection = player.GetSlot(), player
	callScore("Report", report, func(_ []byte, err error) { done(err) })
}

// ReportMatch reports the places of enrolled players together. Done receives
// acceptance or refusal on a later frame. Each place carries a connection.
func ReportMatch(report *MatchReport, done func(error)) {
	callScore("Match", report, func(_ []byte, err error) { done(err) })
}

// ReadBoard reads the board and optional live viewer on a later frame.
func ReadBoard(query *BoardQuery, viewer *Connection, done func(*BoardPage, error)) {
	query = query.CloneVT()
	if viewer != nil {
		query.Player = &viewer.Player
		query.Connection = scoreIdentity(viewer)
	}
	callScore("Read", query, func(reply []byte, err error) {
		if err != nil {
			done(nil, err)
			return
		}
		page := &BoardPage{}
		if err := page.UnmarshalVT(reply); err != nil {
			done(nil, err)
			return
		}
		done(page, nil)
	})
}

// callScore encodes the request before starting a service call.
func callScore(method string, request message, done func([]byte, error)) {
	data, err := request.MarshalVT()
	if err != nil {
		After(0, func() { done(nil, err) })
		return
	}
	CallServiceLater(scoresService, method, data, 0, done)
}

// scoreIdentity copies the fields the host needs to check a live connection.
func scoreIdentity(player *Connection) *ScoreConnection {
	return &ScoreConnection{Slot: player.GetPlayer(), SteamId: player.GetSteamId(), Generation: player.GetGeneration()}
}
