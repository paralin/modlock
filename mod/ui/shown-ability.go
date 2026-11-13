package ui

// ShownAbility is the ability AbilityDisplay shows in one slot.
type ShownAbility struct {
	// Name is the ability's class name, whose window the game shows over it.
	Name string
	// Image is its picture, a path in the game's content as an image's src.
	Image string
	// Upgrades holds its bought tiers as Ability.Upgrades: bit 0 unlocks it.
	Upgrades uint32
}
