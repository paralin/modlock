package mod

// Button bits name inputs for BlockInput, WatchInput and Player.Press.
const (
	// ButtonAttack is the primary fire button.
	ButtonAttack uint64 = 1
	// ButtonJump is the jump key, Space by default (IN_MANTLE). Blocking it
	// stops the native jump and mantle; the jumping client still predicts
	// the jump and is corrected.
	ButtonJump uint64 = 1 << 48
	// ButtonCrouch is the crouch key, Ctrl by default.
	ButtonCrouch uint64 = 1 << 2
	// ButtonDash is the dash key, Shift by default.
	ButtonDash uint64 = 1 << 16
	// ButtonUse is the interact key.
	ButtonUse uint64 = 1 << 5
	// ButtonAltFire is the zoom and alternate fire button, right mouse by
	// default.
	ButtonAltFire uint64 = 1 << 11
	// ButtonReload is the reload key.
	ButtonReload uint64 = 1 << 13
	// ButtonMelee is the light melee key, Q by default.
	ButtonMelee uint64 = 1 << 32
	// ButtonAbility1 to ButtonAbility3 are the hero's first three ability
	// keys.
	ButtonAbility1 uint64 = 1 << 33
	ButtonAbility2 uint64 = 1 << 34
	ButtonAbility3 uint64 = 1 << 35
	// ButtonUltimate is the hero's ultimate ability key, the fourth.
	ButtonUltimate uint64 = 1 << 36
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
	// LayerPlayer is player-controlled heroes.
	LayerPlayer uint64 = 1 << 18
	// LayerNpc is units that are not players.
	LayerNpc uint64 = 1 << 19
	// LayerStaticLevel is the map's built geometry, foliage included.
	LayerStaticLevel uint64 = 1 << 30
	// LayerHero, LayerTrooper and LayerBuilding are those units' bodies.
	LayerHero     uint64 = 1 << 37
	LayerTrooper  uint64 = 1 << 38
	LayerBuilding uint64 = 1 << 40
)
