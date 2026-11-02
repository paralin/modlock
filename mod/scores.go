package mod

import (
	scorespb "github.com/paralin/modlock/src/wasm/scores"
)

// scoresService names the host service that keeps scores.
const scoresService = "scores"

// ScoreReport reports one score to a board the manifest declares; its Player
// is set by ReportScore.
type ScoreReport = scorespb.ScoreReport

// MatchReport reports one match's places to a board the manifest declares.
type MatchReport = scorespb.MatchReport

// BoardQuery selects a page of a board the manifest declares.
type BoardQuery = scorespb.BoardQuery

// BoardPage is a page of a board.
type BoardPage = scorespb.BoardPage

// ReportScore reports report as the player's score over the host's scores
// service. A host without the service takes none; the mod logs the score
// instead, so its author sees what it would report, and returns the host's
// error.
func (p Player) ReportScore(report *ScoreReport) error {
	report.Player = p.Slot
	return reportScores("Report", report, report.GetBoard())
}

// ReportMatch reports one match's places over the host's scores service, as
// ReportScore does. Players who tie share a place.
func ReportMatch(report *MatchReport) error {
	return reportScores("Match", report, report.GetBoard())
}

// ReadBoard reads a page of a board as the host last read it. A page the
// host has not read yet comes back Loading.
func ReadBoard(query *BoardQuery) (*BoardPage, error) {
	data, err := query.MarshalVT()
	if err != nil {
		return nil, err
	}
	reply, err := CallService(scoresService, "Read", data)
	if err != nil {
		return nil, err
	}
	page := &BoardPage{}
	return page, page.UnmarshalVT(reply)
}

// reportScores calls a reporting method and logs the report when the host
// takes no score.
func reportScores(method string, report message, board string) error {
	data, err := report.MarshalVT()
	if err != nil {
		return err
	}
	if _, err := CallService(scoresService, method, data); err != nil {
		Log("score ", board, " not reported: ", err)
		return err
	}
	return nil
}
