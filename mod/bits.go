package mod

// Button bits name inputs for BlockInput, WatchInput and Player.Press.
const (
	// ButtonAttack is the primary fire button.
	ButtonAttack uint64 = 1
	// ButtonUse is the interact key.
	ButtonUse uint64 = 1 << 5
	// ButtonMelee is the light melee button.
	ButtonMelee uint64 = 1 << 11
	// ButtonReload is the reload key.
	ButtonReload uint64 = 1 << 13
	// ButtonAbility1 to ButtonAbility3 are the hero's first three ability
	// keys.
	ButtonAbility1 uint64 = 1 << 33
	ButtonAbility2 uint64 = 1 << 34
	ButtonAbility3 uint64 = 1 << 35
	// ButtonParry is the melee parry button.
	ButtonParry uint64 = 1 << 42
)

// Layer bits name collision layers for Trace.
const (
	// LayerSolid is the world and solid objects.
	LayerSolid uint64 = 1 << 0
	// LayerHitbox is the hit boxes of heroes and units.
	LayerHitbox uint64 = 1 << 1
	// LayerTrigger is invisible volumes that react to touch.
	LayerTrigger uint64 = 1 << 2
	// LayerPlayerClip is walls that only stop heroes.
	LayerPlayerClip uint64 = 1 << 4
	// LayerWorldGeometry is the map's static geometry.
	LayerWorldGeometry uint64 = 1 << 14
	// LayerPlayer is player-controlled heroes.
	LayerPlayer uint64 = 1 << 18
	// LayerNpc is units that are not players.
	LayerNpc uint64 = 1 << 19
	// LayerHero, LayerTrooper and LayerBuilding are those units' bodies.
	LayerHero     uint64 = 1 << 37
	LayerTrooper  uint64 = 1 << 38
	LayerBuilding uint64 = 1 << 40
)
