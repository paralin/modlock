package main

import (
	"strconv"

	"github.com/paralin/modlock/mod"
)

// startColor and checkpointColor are the colors of the course's labels, as
// 0xRRGGBBAA.
const (
	startColor      = 0x5ae65aff
	checkpointColor = 0xffc83cff
)

// labelHeight is how far above a checkpoint its label floats, in units.
const labelHeight = 90

// Course is the checkpoints players marked on the current world and the
// labels that float above them. The first checkpoint is the start line.
type Course struct {
	// checkpoints are the positions in course order.
	checkpoints []*mod.Vector
	// labels are the checkpoints' floating labels, in the same order. A label
	// the game refused is the zero object, and removing it does nothing.
	labels []mod.WorldObject
}

// Len returns how many checkpoints the course has.
func (c *Course) Len() int {
	return len(c.checkpoints)
}

// Checkpoints returns the course's positions in order.
func (c *Course) Checkpoints() []*mod.Vector {
	return c.checkpoints
}

// Add appends a checkpoint at position and labels it: Start for the first,
// then its number. When the game refuses the label, Add keeps the checkpoint
// without one.
func (c *Course) Add(position *mod.Vector) {
	// Pick the label's words and color.
	text, color := strconv.Itoa(len(c.checkpoints)), uint32(checkpointColor)
	if len(c.checkpoints) == 0 {
		text, color = "Start", startColor
	}

	// Float the label above the checkpoint, facing each viewer.
	faceCamera := true
	size := float32(120)
	label, err := mod.CreateText(&mod.TextOptions{
		Text:       text,
		Position:   above(position, labelHeight),
		FontSize:   &size,
		Color:      &color,
		FaceCamera: &faceCamera,
	})
	if err != nil {
		mod.Log("race: cannot label checkpoint ", text, ": ", err)
	}

	// Keep the checkpoint with its label.
	c.checkpoints = append(c.checkpoints, position)
	c.labels = append(c.labels, label)
}

// RemoveLast removes the newest checkpoint and its label, and reports
// whether there was one.
func (c *Course) RemoveLast() bool {
	// Find the newest checkpoint, if any.
	last := len(c.checkpoints) - 1
	if last < 0 {
		return false
	}

	// Remove its label, then the checkpoint.
	c.remove(c.labels[last])
	c.checkpoints = c.checkpoints[:last]
	c.labels = c.labels[:last]
	return true
}

// Clear removes every checkpoint and label.
func (c *Course) Clear() {
	for _, label := range c.labels {
		c.remove(label)
	}
	c.Forget()
}

// Forget clears the checkpoints and the course's record of its labels without
// removing the labels from the game, because the game already removed them
// when it loaded the new map.
func (c *Course) Forget() {
	c.checkpoints = nil
	c.labels = nil
}

// remove takes one label out of the world, skipping one the game refused.
func (c *Course) remove(label mod.WorldObject) {
	if label != (mod.WorldObject{}) {
		_ = label.Remove()
	}
}
