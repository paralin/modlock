package mod

import (
	"errors"

	entitiespb "github.com/paralin/modlock/src/wasm/entities"
)

const (
	cameraOverrideModifier     = "modifier_citadel_hideout_intro"
	cameraOverrideExitModifier = "modifier_citadel_hideout_intro_exit"
	primaryCameraName          = "intro_camera"
	preIntroCameraName         = "pre_intro_camera"
)

// EntityCameraSettleSeconds is how long an entity camera takes to settle
// after it opens. The game client blends into intro_camera from a snapshot of
// the opening view for about five seconds, interpolating pitch, yaw and roll
// as wrapped angles, so an orientation that crosses 180 degrees of roll, or
// passes over the top in pitch, during the blend swings the view the wrong
// way and stalls it. Hold the camera clear of those crossings, such as level,
// for this long after OpenEntityCamera; from then on any orientation renders
// exactly.
const EntityCameraSettleSeconds = 6

// SetOrbitCamera gives the player's hero the hideout's orbit camera, zoomed
// with the wheel, or returns the normal camera. The hideout HUD replaces the
// game HUD while it is on.
//
// The pawn's networked m_bInHideoutZone moves its client to the hideout view,
// and the wheel zooms only while the pawn holds MODIFIER_STATE_HIDEOUT_ZONE,
// since the server ignores orbit_camera_in and orbit_camera_out without it.
func SetOrbitCamera(player Player, enabled bool) error {
	pawn, err := player.Pawn()
	if err != nil {
		return err
	}
	entity := pawn.GetEntity()
	if entity == 0 {
		return errors.New("the player has no hero")
	}

	// The flag picks the view; the held state lets the wheel zoom it.
	return errors.Join(
		WriteField(entity, "CCitadelPlayerPawn", "m_bInHideoutZone", FieldTypeBool, enabled),
		HoldModifierState(entity, "MODIFIER_STATE_HIDEOUT_ZONE", &enabled),
	)
}

// EntityCameraOptions configures a player's 6-DOF point_camera override.
type EntityCameraOptions struct {
	Position Vector
	Facing   Angles
	// FOV is the horizontal field of view in degrees; zero uses 90.
	FOV float32
	// ZNear and ZFar override clipping planes when non-zero.
	ZNear float32
	ZFar  float32
	// Smooth skips the frame-1 pre_intro_camera so the game blends in over 0.5s.
	Smooth bool
}

// EntityCamera drives one player's 6-DOF point_camera override while keeping
// their living hero active.
type EntityCamera struct {
	player    Player
	entity    uint32
	preEntity uint32
	// heldPawn is the pawn holding the modifier itself, or 0 for a player hold.
	heldPawn uint32
	stopped  bool
}

// Entity returns the player's primary point_camera entity handle.
func (c *EntityCamera) Entity() uint32 {
	return c.entity
}

func cameraKeyValues(targetname string, opts EntityCameraOptions) []*entitiespb.KeyValue {
	fov := opts.FOV
	if fov == 0 {
		fov = 90
	}
	keys := []*entitiespb.KeyValue{
		{Key: "targetname", Value: &entitiespb.EntityValue{Value: &entitiespb.EntityValue_Text{Text: targetname}}},
		{Key: "FOV", Value: &entitiespb.EntityValue{Value: &entitiespb.EntityValue_Number{Number: fov}}},
		{Key: "UseScreenAspectRatio", Value: &entitiespb.EntityValue{Value: &entitiespb.EntityValue_Boolean{Boolean: true}}},
		{Key: "align_with_parent", Value: &entitiespb.EntityValue{Value: &entitiespb.EntityValue_Boolean{Boolean: false}}},
	}
	if opts.ZNear != 0 {
		keys = append(keys, &entitiespb.KeyValue{Key: "zNear", Value: &entitiespb.EntityValue{Value: &entitiespb.EntityValue_Number{Number: opts.ZNear}}})
	}
	if opts.ZFar != 0 {
		keys = append(keys, &entitiespb.KeyValue{Key: "zFar", Value: &entitiespb.EntityValue{Value: &entitiespb.EntityValue_Number{Number: opts.ZFar}}})
	}
	return keys
}

func spawnPointCamera(slot int32, targetname string, opts EntityCameraOptions) (uint32, error) {
	always := true
	pos := opts.Position
	facing := opts.Facing
	entity, err := CreateEntity(&EntityOptions{
		DesignerName:   "point_camera",
		Position:       &pos,
		Facing:         &facing,
		TransmitAlways: &always,
		KeyValues:      cameraKeyValues(targetname, opts),
	})
	if err != nil {
		return 0, err
	}
	if err := ShowEntityOnlyTo(entity, []int32{slot}); err != nil {
		_, _ = RemoveEntity(entity)
		return 0, err
	}
	return entity, nil
}

// OpenEntityCamera gives player's living hero a per-player 6-DOF point_camera
// view that preserves full 360-degree roll and over-the-top pitch loops while
// keeping WatchMovement analog input active.
func OpenEntityCamera(player Player, opts EntityCameraOptions) (*EntityCamera, error) {
	pawn, err := player.Pawn()
	if err != nil {
		return nil, err
	}
	if pawn.GetEntity() == 0 {
		return nil, errors.New("the player has no hero")
	}

	entity, err := spawnPointCamera(player.Slot(), primaryCameraName, opts)
	if err != nil {
		return nil, err
	}
	var preEntity uint32
	if !opts.Smooth {
		preEntity, err = spawnPointCamera(player.Slot(), preIntroCameraName, opts)
		if err != nil {
			_, _ = RemoveEntity(entity)
			return nil, err
		}
	}

	// A player hold survives respawns but needs an owned ability to apply the
	// modifier, which a freshly spawned hero may not have yet; the pawn's own
	// hold needs none.
	active := true
	var heldPawn uint32
	if err := player.HoldModifier(cameraOverrideModifier, &active); err != nil {
		if pawnErr := HoldEntityModifier(pawn.GetEntity(), cameraOverrideModifier, &active); pawnErr != nil {
			if preEntity != 0 {
				_, _ = RemoveEntity(preEntity)
			}
			_, _ = RemoveEntity(entity)
			return nil, errors.Join(err, pawnErr)
		}
		heldPawn = pawn.GetEntity()
	}
	return &EntityCamera{player: player, entity: entity, preEntity: preEntity, heldPawn: heldPawn}, nil
}

// Move places the camera at position and facing, preserving full roll.
func (c *EntityCamera) Move(position Vector, facing Angles) error {
	if c.stopped {
		return errors.New("entity camera is stopped")
	}
	err := MoveEntity(c.entity, &position, &facing, nil)
	if c.preEntity != 0 {
		_ = MoveEntity(c.preEntity, &position, &facing, nil)
	}
	return err
}

// SetFOV updates the camera's horizontal field of view in degrees.
func (c *EntityCamera) SetFOV(fov float32) error {
	if c.stopped {
		return errors.New("entity camera is stopped")
	}
	err := WriteField(c.entity, "CPointCamera", "m_FOV", FieldTypeFloat32, fov)
	if c.preEntity != 0 {
		_ = WriteField(c.preEntity, "CPointCamera", "m_FOV", FieldTypeFloat32, fov)
	}
	return err
}

// Stop removes the camera entities and modifier and returns to the hero camera.
func (c *EntityCamera) Stop() error {
	if c.stopped {
		return nil
	}
	c.stopped = true
	inactive := false
	var errs []error
	if c.heldPawn != 0 {
		errs = append(errs, HoldEntityModifier(c.heldPawn, cameraOverrideModifier, &inactive))
	} else {
		errs = append(errs, c.player.HoldModifier(cameraOverrideModifier, &inactive))
	}
	if pawn, err := c.player.Pawn(); err == nil && pawn.GetEntity() != 0 {
		_, err1 := FireInput(pawn.GetEntity(), "RemoveModifier", cameraOverrideModifier, nil)
		_, err2 := FireInput(pawn.GetEntity(), "RemoveModifier", cameraOverrideExitModifier, nil)
		errs = append(errs, err1, err2)
	}
	if c.preEntity != 0 {
		_, err := RemoveEntity(c.preEntity)
		errs = append(errs, err)
		c.preEntity = 0
	}
	_, err := RemoveEntity(c.entity)
	errs = append(errs, err)
	return errors.Join(errs...)
}
