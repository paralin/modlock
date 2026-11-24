package boards

import (
	"strings"
	"testing"

	"github.com/paralin/modlock/src/wasm"
	scorespb "github.com/paralin/modlock/src/wasm/scores"
)

// TestValidate accepts well formed boards and refuses each malformed
// declaration a mod author could write, then checks a score's partition
// values against the declared ones.
func TestValidate(t *testing.T) {
	// A best time per course, kept all time and each week.
	trial := func() *scorespb.Board {
		return &scorespb.Board{
			Name: "trial.time", Unit: scorespb.Board_UNIT_MILLISECONDS, Order: scorespb.Board_ORDER_ASCENDING,
			Keep: scorespb.Board_KEEP_BEST, Windows: []scorespb.Window{scorespb.Window_WINDOW_ALL_TIME, scorespb.Window_WINDOW_WEEK},
			Partitions: []*scorespb.Partition{{Name: "course", Values: []string{"canyon", "mesa"}}},
		}
	}
	rating := &scorespb.Board{Name: "duel", Unit: scorespb.Board_UNIT_NUMBER, Order: scorespb.Board_ORDER_DESCENDING,
		Keep: scorespb.Board_KEEP_RATING, Rating: scorespb.Board_RATING_ELO}
	if err := Validate(&wasm.Manifest{Boards: []*scorespb.Board{trial(), rating}}); err != nil {
		t.Fatalf("valid boards refused: %v", err)
	}

	// Each change makes the board invalid, with the error naming why.
	for want, change := range map[string]func(*scorespb.Board){
		"name must be":               func(b *scorespb.Board) { b.Name = "9lives" },
		"unit must be":               func(b *scorespb.Board) { b.Unit = scorespb.Board_UNIT_UNKNOWN },
		"order must be":              func(b *scorespb.Board) { b.Order = scorespb.Board_ORDER_UNKNOWN },
		"only KEEP_RATING":           func(b *scorespb.Board) { b.Rating = scorespb.Board_RATING_ELO },
		"rating must be":             func(b *scorespb.Board) { b.Keep = scorespb.Board_KEEP_RATING },
		"twice":                      func(b *scorespb.Board) { b.Windows = append(b.Windows, scorespb.Window_WINDOW_WEEK) },
		"window must be":             func(b *scorespb.Board) { b.Windows = []scorespb.Window{scorespb.Window_WINDOW_UNKNOWN} },
		"lowest is above":            func(b *scorespb.Board) { low, high := int64(9), int64(1); b.Lowest, b.Highest = &low, &high },
		"must be a short":            func(b *scorespb.Board) { b.Partitions[0].Values = []string{"two words"} },
		"partition \"course\" twice": func(b *scorespb.Board) { b.Partitions = append(b.Partitions, b.Partitions[0]) },
		"teamSize must be":           func(b *scorespb.Board) { b.TeamSize = MaxTeamSize + 1 },
	} {
		board := trial()
		change(board)
		err := Validate(&wasm.Manifest{Boards: []*scorespb.Board{board}})
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: got %v", want, err)
		}
	}
	if err := Validate(&wasm.Manifest{Boards: []*scorespb.Board{trial(), trial()}}); err == nil {
		t.Error("a board declared twice was accepted")
	}
	teamRating := &scorespb.Board{Name: "duel", Unit: scorespb.Board_UNIT_NUMBER, Order: scorespb.Board_ORDER_DESCENDING,
		Keep: scorespb.Board_KEEP_RATING, Rating: scorespb.Board_RATING_ELO, TeamSize: 2}
	if err := Validate(&wasm.Manifest{Boards: []*scorespb.Board{teamRating}}); err == nil || !strings.Contains(err.Error(), "keep no rating") {
		t.Errorf("a rated team board: got %v", err)
	}

	// A team board takes the team's other players, each once; a board of
	// single players takes none.
	pairs := trial()
	pairs.TeamSize = 2
	if err := Teamed(pairs, 0, []int32{3}); err != nil {
		t.Errorf("a pair refused: %v", err)
	}
	for _, teammates := range [][]int32{nil, {3, 4}, {0}} {
		if Teamed(pairs, 0, teammates) == nil {
			t.Errorf("pair board took teammates %v", teammates)
		}
	}
	if Teamed(trial(), 0, []int32{3}) == nil {
		t.Error("a board of single players took a teammate")
	}
	if err := Teamed(trial(), 0, nil); err != nil {
		t.Errorf("a single player refused: %v", err)
	}

	// A score names one declared value for each partition.
	board := trial()
	if err := Partitioned(board, map[string]string{"course": "mesa"}); err != nil {
		t.Errorf("declared course refused: %v", err)
	}
	for _, values := range []map[string]string{nil, {"course": "dune"}, {"course": "mesa", "lap": "1"}, {"lap": "1"}} {
		if Partitioned(board, values) == nil {
			t.Errorf("partition %v accepted", values)
		}
	}
}
