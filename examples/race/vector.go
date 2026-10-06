package main

import (
	"math"

	"github.com/paralin/modlock/mod"
)

// distance returns the straight-line distance between a and b, in units.
func distance(a, b *mod.Vector) float64 {
	return math.Hypot(math.Hypot(a.GetX()-b.GetX(), a.GetY()-b.GetY()), a.GetZ()-b.GetZ())
}

// above returns the point height units above position.
func above(position *mod.Vector, height float64) *mod.Vector {
	return &mod.Vector{X: position.GetX(), Y: position.GetY(), Z: position.GetZ() + height}
}

// toward returns the level facing that looks from one point toward another.
func toward(from, to *mod.Vector) *mod.Angles {
	return &mod.Angles{Yaw: math.Atan2(to.GetY()-from.GetY(), to.GetX()-from.GetX()) * 180 / math.Pi}
}
