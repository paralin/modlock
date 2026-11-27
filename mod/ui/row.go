package ui

// Row is one row of a List.
type Row struct {
	// Key names the row, 1 to 64 bytes.
	Key string
	// Rank orders the rows, least first.
	Rank float64
	// Sort orders the rows of one rank, as text.
	Sort string
	// Fields are what the template's "{field}" names show, at most sixteen,
	// each named by letters, digits and '_'.
	Fields map[string]string
}
