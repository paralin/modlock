package project

import (
	"path/filepath"
	"testing"

	"github.com/evanw/esbuild/pkg/api"
	"github.com/paralin/modlock/proto/modlock/cli"
)

// TestDiagnostics checks that each tool's output becomes diagnostics with
// absolute paths and 1-based columns.
func TestDiagnostics(t *testing.T) {
	dir := filepath.FromSlash("/mods/demo")
	main := filepath.Join(dir, "main.ts")
	tests := []struct {
		name string
		got  []*cli.Diagnostic
		want []*cli.Diagnostic
	}{
		{
			name: "tsgo",
			got: tsgoDiagnostics(dir, []byte("main.ts(7,29): error TS2322: Type 'string' is not assignable to type 'number'.\n"+
				"  The expected type comes from here.\n"+
				"Found 1 error in main.ts:7\n")),
			want: []*cli.Diagnostic{{
				File: main, Line: 7, Column: 29, Source: "tsgo", Code: "TS2322",
				Message: "Type 'string' is not assignable to type 'number'.\nThe expected type comes from here.",
			}},
		},
		{
			name: "go",
			got: goDiagnostics(dir, "go vet", []byte("# demo\n"+
				"vet: ./main.go:17:28: cannot use \"s\" as int value\n"+
				"main.go:19: missing return\n")),
			want: []*cli.Diagnostic{
				{File: filepath.Join(dir, "main.go"), Line: 17, Column: 28, Source: "go vet", Message: `cannot use "s" as int value`},
				{File: filepath.Join(dir, "main.go"), Line: 19, Source: "go vet", Message: "missing return"},
			},
		},
		{
			name: "luau-analyze",
			got: luauDiagnostics(dir, []byte("./main.luau(4,2): TypeError: Key 'nope' not found in table 'Player'\n"+
				"the mod has type errors\n")),
			want: []*cli.Diagnostic{{
				File: filepath.Join(dir, "main.luau"), Line: 4, Column: 2, Source: "luau-analyze", Code: "TypeError",
				Message: "Key 'nope' not found in table 'Player'",
			}},
		},
		{
			name: "pyright",
			got: pyrightDiagnostics([]byte(`{"generalDiagnostics": [` +
				`{"file": "/mods/demo/main.py", "severity": "error", "message": "Cannot access attribute \"nope\"", ` +
				`"range": {"start": {"line": 3, "character": 4}}, "rule": "reportAttributeAccessIssue"}, ` +
				`{"file": "/mods/demo/main.py", "severity": "information", "message": "note", "range": {"start": {"line": 0, "character": 0}}}]}`)),
			want: []*cli.Diagnostic{{
				File: "/mods/demo/main.py", Line: 4, Column: 5, Source: "pyright", Code: "reportAttributeAccessIssue",
				Message: `Cannot access attribute "nope"`,
			}},
		},
		{
			name: "esbuild",
			got: esbuildDiagnostics(dir, []api.Message{{
				Text:     `Could not resolve "./missing"`,
				Location: &api.Location{File: "main.ts", Line: 1, Column: 18},
			}}),
			want: []*cli.Diagnostic{{File: main, Line: 1, Column: 19, Source: "esbuild", Message: `Could not resolve "./missing"`}},
		},
	}
	for _, test := range tests {
		if len(test.got) != len(test.want) {
			t.Fatalf("%s: got %d diagnostics, want %d", test.name, len(test.got), len(test.want))
		}
		for i := range test.want {
			if !test.got[i].EqualVT(test.want[i]) {
				t.Errorf("%s: got %v, want %v", test.name, test.got[i], test.want[i])
			}
		}
	}
}
