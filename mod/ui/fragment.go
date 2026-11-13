package ui

// Fragment groups children without a panel of its own, and keys a component
// in a list: ui.Fragment{Key: id, Children: Row{...}}.
type Fragment struct {
	// Key keeps the fragment's nodes in a list while the list changes.
	Key string
	// Children are the nodes the fragment holds.
	Children Node
}

// Render returns the fragment's children.
func (f Fragment) Render() Node {
	return f.Children
}

// key returns the fragment's key.
func (f Fragment) key() string {
	return f.Key
}
