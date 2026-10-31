package session

import "testing"

// TestLastAccount reads the account signed in last from loginusers.vdf as
// older clients write it, with a MostRecent mark, and as current clients do,
// with only sign-in times.
func TestLastAccount(t *testing.T) {
	marked := []byte(`"users"
{
	"76561198000000001"
	{
		"AccountName"		"first"
		"MostRecent"		"0"
		"Timestamp"		"1700000009"
	}
	"76561198000000002"
	{
		"AccountName"		"second"
		"MostRecent"		"1"
		"Timestamp"		"1700000001"
	}
}`)
	if got := lastAccount(marked); got != 76561198000000002 {
		t.Fatalf("marked: got %d, want the MostRecent account", got)
	}

	unmarked := []byte(`"users"
{
	"76561198000000001"
	{
		"AccountName"		"first"
		"timestamp"		"1793402493"
	}
	"76561198000000002"
	{
		"AccountName"		"second"
		"timestamp"		"1700000001"
	}
}`)
	if got := lastAccount(unmarked); got != 76561198000000001 {
		t.Fatalf("unmarked: got %d, want the latest sign-in", got)
	}
	if got := lastAccount([]byte(`"users" {}`)); got != 0 {
		t.Fatalf("empty: got %d, want zero", got)
	}
}
