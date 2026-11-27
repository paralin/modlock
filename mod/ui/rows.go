package ui

import (
	"cmp"
	"slices"
)

// Rows are a list's rows in order, which a List draws. A Rows does not
// change; to change a list's rows, draw it with new Rows.
type Rows struct {
	// order lists the rows by rank, then sort, then key.
	order []Row
	// at maps each key to its row's place in order.
	at map[string]int
}

// NewRows orders rows by Rank, then Sort, then Key, keeping the last row of
// each key. The host takes at most 20,000 rows in a list. Rows already in that order with no key
// twice are kept as they are, so ordering them costs one pass.
func NewRows(rows ...Row) *Rows {
	// Index the keys, noting whether the rows are in order with no key twice.
	at := make(map[string]int, len(rows))
	sorted := true
	for i, row := range rows {
		if _, ok := at[row.Key]; ok || (i > 0 && compareRows(rows[i-1], row) > 0) {
			sorted = false
		}
		at[row.Key] = i
	}

	// Otherwise keep each key's last row, in order.
	order := slices.Clone(rows)
	if !sorted {
		order = order[:0]
		for i, row := range rows {
			if at[row.Key] == i {
				order = append(order, row)
			}
		}
		slices.SortFunc(order, compareRows)
	}
	clear(at)
	for i, row := range order {
		at[row.Key] = i
	}
	return &Rows{order: order, at: at}
}

// Len returns how many rows there are.
func (r *Rows) Len() int {
	if r == nil {
		return 0
	}
	return len(r.order)
}

// compareRows orders two rows by rank, then sort, then key.
func compareRows(a, b Row) int {
	return cmp.Or(cmp.Compare(a.Rank, b.Rank), cmp.Compare(a.Sort, b.Sort), cmp.Compare(a.Key, b.Key))
}
