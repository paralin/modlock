package sandbox

import (
	"sort"
	"strconv"
	"strings"

	"github.com/paralin/modlock/boards"
	"github.com/paralin/modlock/src/wasm"
	scorespb "github.com/paralin/modlock/src/wasm/scores"
)

// scoresService names the service mods report scores to.
const scoresService = "scores"

// scores answers the scores service as a host without a backend: it checks
// each report against the boards manifest declares and logs it as it would be
// reported, and reads every board as empty. A report the declaration refuses
// fails, so the mod's author sees why.
func (s *Sandbox) scores(manifest *wasm.Manifest, call *wasm.ServiceCall) *wasm.Reply {
	// Decode the method's request and find its board.
	mod := manifest.GetSlug()
	var board string
	var partition map[string]string
	var line string
	var response []byte
	// team is the call's player and teammates when it names a team's row:
	// every report, and a read that asks for the viewer's own row.
	var team []int32
	teamed := false
	switch call.GetMethod() {
	case "Report":
		report := &scorespb.ScoreReport{}
		if err := report.UnmarshalVT(call.GetPayload()); err != nil {
			return &wasm.Reply{Error: "the scores Report request is malformed"}
		}
		board, partition = report.GetBoard(), report.GetPartition()
		team, teamed = teamSlots(report.GetPlayer(), report.GetTeammates()), true
		line = describeTeam(team) + " " + strconv.FormatInt(report.GetValue(), 10)
	case "Match":
		match := &scorespb.MatchReport{}
		if err := match.UnmarshalVT(call.GetPayload()); err != nil {
			return &wasm.Reply{Error: "the scores Match request is malformed"}
		}
		board, partition = match.GetBoard(), match.GetPartition()
		places := make([]string, 0, len(match.GetPlaces()))
		for _, place := range match.GetPlaces() {
			places = append(places, "player "+strconv.Itoa(int(place.GetPlayer()))+" #"+strconv.Itoa(int(place.GetPlace())))
		}
		line = strings.Join(places, ", ")
	case "Read":
		query := &scorespb.BoardQuery{}
		if err := query.UnmarshalVT(call.GetPayload()); err != nil {
			return &wasm.Reply{Error: "the scores Read request is malformed"}
		}
		board, partition = query.GetBoard(), query.GetPartition()
		team, teamed = teamSlots(query.GetPlayer(), query.GetTeammates()), query.Player != nil || len(query.GetTeammates()) != 0
		response, _ = (&scorespb.BoardPage{}).MarshalVT()
	default:
		return &wasm.Reply{Error: "the scores service has no method " + call.GetMethod()}
	}

	// Check the board and partition the call names.
	declared := boards.Find(manifest, board)
	if declared == nil {
		return s.refuse(mod, "mod.json declares no board "+board)
	}
	if err := boards.Partitioned(declared, partition); err != nil {
		return s.refuse(mod, err.Error())
	}
	if teamed {
		if err := boards.Teamed(declared, team[0], team[1:]); err != nil {
			return s.refuse(mod, err.Error())
		}
	}

	// Log a report as it would be reported.
	if line != "" {
		s.log(mod, "score "+board+describePartition(partition)+": "+line)
	}
	reply, _ := (&wasm.ServiceReply{Payload: response}).MarshalVT()
	return &wasm.Reply{Response: reply}
}

// teamSlots lists a call's player then its teammates by slot.
func teamSlots(player int32, teammates []*scorespb.Teammate) []int32 {
	team := []int32{player}
	for _, teammate := range teammates {
		team = append(team, teammate.GetPlayer())
	}
	return team
}

// describeTeam writes a call's players as "player 1" or "players 1, 2".
func describeTeam(team []int32) string {
	if len(team) == 1 {
		return "player " + strconv.Itoa(int(team[0]))
	}
	slots := make([]string, len(team))
	for i, slot := range team {
		slots[i] = strconv.Itoa(int(slot))
	}
	return "players " + strings.Join(slots, ", ")
}

// refuse logs and fails a scores call.
func (s *Sandbox) refuse(mod, text string) *wasm.Reply {
	s.log(mod, text)
	return &wasm.Reply{Error: text}
}

// describePartition writes partition values as "[course=canyon]", in name
// order, or nothing when there are none.
func describePartition(partition map[string]string) string {
	if len(partition) == 0 {
		return ""
	}
	names := make([]string, 0, len(partition))
	for name := range partition {
		names = append(names, name)
	}
	sort.Strings(names)
	for i, name := range names {
		names[i] = name + "=" + partition[name]
	}
	return "[" + strings.Join(names, " ") + "]"
}
