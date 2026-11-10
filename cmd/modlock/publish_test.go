package main

import (
	"testing"

	"github.com/paralin/modlock/src/wasm"
)

// TestCheckRuntime checks that publish refuses a RUNTIME_WASM mod built for
// QuickJS unless the fallback is allowed, and passes every other build.
func TestCheckRuntime(t *testing.T) {
	wasmRuntime, quickjs := wasm.Manifest_RUNTIME_WASM, wasm.Manifest_RUNTIME_QUICKJS
	cases := []struct {
		name          string
		asked, built  wasm.Manifest_Runtime
		allowFallback bool
		refused       bool
	}{
		{"compiled", wasmRuntime, wasmRuntime, false, false},
		{"fell back", wasmRuntime, quickjs, false, true},
		{"fell back, allowed", wasmRuntime, quickjs, true, false},
		{"asks for QuickJS", quickjs, quickjs, false, false},
		{"asks for nothing", wasm.Manifest_RUNTIME_UNKNOWN, quickjs, false, false},
	}
	for _, c := range cases {
		err := checkRuntime(c.asked, c.built, c.allowFallback)
		if (err != nil) != c.refused {
			t.Errorf("%s: checkRuntime returned %v, want refused %v", c.name, err, c.refused)
		}
	}
}
