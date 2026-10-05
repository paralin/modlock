package mod

import (
	"bytes"
	"testing"
)

// TestSpotRoundTrip checks that a spot survives its document and that equal
// spots encode to equal bytes, with each model and builder stored once.
func TestSpotRoundTrip(t *testing.T) {
	crate := "models/props/crate.vmdl"
	s := &Spot{Map: "dl_midtown", Objects: []*SpotObject{
		{Model: crate, Position: &Vector{X: 8, Y: -16, Z: 24.5}, Facing: &Angles{Yaw: 90}, Scale: 1, PlacedBy: 7},
		{Model: crate, Position: &Vector{X: 1, Y: 2, Z: 3}, Facing: &Angles{}, Scale: 2, Bounce: 900, PlacedBy: 7},
		{Model: "models/props/ramp.vmdl", Position: &Vector{}, Facing: &Angles{Roll: 15}, Scale: 1},
	}}
	encoded, err := EncodeSpot(s)
	if err != nil {
		t.Fatal(err)
	}
	again, _ := EncodeSpot(s)
	if !bytes.Equal(encoded, again) {
		t.Fatal("equal spots encoded to different bytes")
	}
	if n := bytes.Count(encoded, []byte(crate)); n != 1 {
		t.Fatalf("the crate's path is stored %d times, want once", n)
	}

	decoded, err := DecodeSpot(encoded)
	if err != nil {
		t.Fatal(err)
	}
	if decoded.Map != s.Map || len(decoded.Objects) != len(s.Objects) {
		t.Fatalf("decoded %v, want %v", decoded, s)
	}
	for i, want := range s.Objects {
		got := decoded.Objects[i]
		if got.Model != want.Model || got.Scale != want.Scale || got.Bounce != want.Bounce || got.PlacedBy != want.PlacedBy ||
			!got.Position.EqualVT(want.Position) || !got.Facing.EqualVT(want.Facing) {
			t.Errorf("object %d decoded as %+v, want %+v", i, got, want)
		}
	}
}
