// Package boards holds the rules for the boards a mod declares in its
// manifest: which declarations a build may carry, and which partition values a
// score may name. A host that serves the scores service checks reports with
// the same rules.
package boards

import (
	"regexp"
	"slices"

	manifestpb "github.com/paralin/modlock/src/wasm/manifest"
	scorespb "github.com/paralin/modlock/src/wasm/scores"
	"github.com/pkg/errors"
)

const (
	// limit bounds how many boards a mod declares.
	limit = 32
	// partitionLimit bounds the partitions of one board, so a score updates
	// a bounded number of rankings.
	partitionLimit = 4
	// valueLimit bounds the declared values of one partition.
	valueLimit = 256
)

var (
	// namePattern matches a board name.
	namePattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_.]{0,63}$`)
	// partitionPattern matches a partition name.
	partitionPattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_.]{0,31}$`)
	// valuePattern matches a partition value.
	valuePattern = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,64}$`)
)

// Find returns the board named name that manifest declares, or nil.
func Find(manifest *manifestpb.Manifest, name string) *scorespb.Board {
	for _, board := range manifest.GetBoards() {
		if board.GetName() == name {
			return board
		}
	}
	return nil
}

// Windows returns the windows board keeps standings for: its declared
// windows, or all time alone when it declares none.
func Windows(board *scorespb.Board) []scorespb.Window {
	if len(board.GetWindows()) == 0 {
		return []scorespb.Window{scorespb.Window_WINDOW_ALL_TIME}
	}
	return board.GetWindows()
}

// Validate checks the boards manifest declares.
func Validate(manifest *manifestpb.Manifest) error {
	boards := manifest.GetBoards()
	if len(boards) > limit {
		return errors.Errorf("mod.json declares more than %d boards", limit)
	}
	for i, board := range boards {
		if err := validate(board); err != nil {
			return err
		}
		if slices.ContainsFunc(boards[:i], func(b *scorespb.Board) bool { return b.GetName() == board.GetName() }) {
			return errors.Errorf("mod.json declares board %q twice", board.GetName())
		}
	}
	return nil
}

// validate checks one declared board.
func validate(board *scorespb.Board) error {
	// Check the name the code reports to.
	name := board.GetName()
	if !namePattern.MatchString(name) {
		return errors.Errorf("board %q name must be up to 64 letters, digits, underscores and dots, starting with a letter or underscore", name)
	}

	// Check how the board ranks and keeps values.
	switch board.GetUnit() {
	case scorespb.Board_UNIT_NUMBER, scorespb.Board_UNIT_MILLISECONDS:
	default:
		return errors.Errorf("board %q unit must be UNIT_NUMBER or UNIT_MILLISECONDS", name)
	}
	switch board.GetOrder() {
	case scorespb.Board_ORDER_DESCENDING, scorespb.Board_ORDER_ASCENDING:
	default:
		return errors.Errorf("board %q order must be ORDER_DESCENDING or ORDER_ASCENDING", name)
	}
	switch board.GetKeep() {
	case scorespb.Board_KEEP_BEST, scorespb.Board_KEEP_SUM, scorespb.Board_KEEP_COUNT, scorespb.Board_KEEP_LATEST:
		if board.GetRating() != scorespb.Board_RATING_UNKNOWN {
			return errors.Errorf("board %q names a rating, which only KEEP_RATING takes", name)
		}
	case scorespb.Board_KEEP_RATING:
		switch board.GetRating() {
		case scorespb.Board_RATING_ELO, scorespb.Board_RATING_GLICKO2:
		default:
			return errors.Errorf("board %q rating must be RATING_ELO or RATING_GLICKO2", name)
		}
		if board.GetOrder() != scorespb.Board_ORDER_DESCENDING {
			return errors.Errorf("board %q ranks a rating, which must be ORDER_DESCENDING", name)
		}
	default:
		return errors.Errorf("board %q keep must be KEEP_BEST, KEEP_SUM, KEEP_COUNT, KEEP_LATEST or KEEP_RATING", name)
	}
	if board.Lowest != nil && board.Highest != nil && board.GetLowest() > board.GetHighest() {
		return errors.Errorf("board %q lowest is above its highest", name)
	}

	// Check the windows, tier and HUD.
	windows := board.GetWindows()
	for i, window := range windows {
		switch window {
		case scorespb.Window_WINDOW_ALL_TIME, scorespb.Window_WINDOW_DAY, scorespb.Window_WINDOW_WEEK:
		default:
			return errors.Errorf("board %q window must be WINDOW_ALL_TIME, WINDOW_DAY or WINDOW_WEEK", name)
		}
		if slices.Contains(windows[:i], window) {
			return errors.Errorf("board %q lists window %s twice", name, window)
		}
	}
	switch board.GetTier() {
	case scorespb.Tier_TIER_UNKNOWN, scorespb.Tier_TIER_HOSTED, scorespb.Tier_TIER_ADMITTED, scorespb.Tier_TIER_DEDICATED:
	default:
		return errors.Errorf("board %q tier must be TIER_HOSTED, TIER_ADMITTED or TIER_DEDICATED", name)
	}
	switch board.GetHud() {
	case scorespb.Hud_HUD_UNKNOWN, scorespb.Hud_HUD_END_CARD:
	default:
		return errors.Errorf("board %q hud must be HUD_END_CARD or unset", name)
	}

	// Check the partitions and the values each accepts.
	partitions := board.GetPartitions()
	if len(partitions) > partitionLimit {
		return errors.Errorf("board %q declares more than %d partitions", name, partitionLimit)
	}
	for i, partition := range partitions {
		if !partitionPattern.MatchString(partition.GetName()) {
			return errors.Errorf("board %q partition %q name must be up to 32 letters, digits, underscores and dots, starting with a letter or underscore", name, partition.GetName())
		}
		if slices.ContainsFunc(partitions[:i], func(p *scorespb.Partition) bool { return p.GetName() == partition.GetName() }) {
			return errors.Errorf("board %q declares partition %q twice", name, partition.GetName())
		}
		values := partition.GetValues()
		if len(values) > valueLimit {
			return errors.Errorf("board %q partition %q lists more than %d values", name, partition.GetName(), valueLimit)
		}
		for j, value := range values {
			if !valuePattern.MatchString(value) {
				return errors.Errorf("board %q partition %q value %q must be a short word", name, partition.GetName(), value)
			}
			if slices.Contains(values[:j], value) {
				return errors.Errorf("board %q partition %q lists value %q twice", name, partition.GetName(), value)
			}
		}
	}
	return nil
}

// Partitioned checks that values names one accepted value for each of
// board's partitions and nothing else.
func Partitioned(board *scorespb.Board, values map[string]string) error {
	partitions := board.GetPartitions()
	if len(values) != len(partitions) {
		return errors.Errorf("board %q takes %d partition values, not %d", board.GetName(), len(partitions), len(values))
	}
	for _, partition := range partitions {
		value, ok := values[partition.GetName()]
		if !ok {
			return errors.Errorf("board %q needs a %s", board.GetName(), partition.GetName())
		}
		if !valuePattern.MatchString(value) {
			return errors.Errorf("board %q %s %q must be a short word", board.GetName(), partition.GetName(), value)
		}
		if declared := partition.GetValues(); len(declared) != 0 && !slices.Contains(declared, value) {
			return errors.Errorf("board %q has no %s %q", board.GetName(), partition.GetName(), value)
		}
	}
	return nil
}
