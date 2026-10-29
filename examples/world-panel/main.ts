// world-panel raises a board of the mod's own layout in front of each player
// as their hero first spawns. The layout lives in content/layout/board.xml with
// its style sheet in content/styles/board.css, and both ship in the mod's
// content addon, which the host names in place of {addon}.
import { log, onFrame, players } from 'modlock'
import { create } from 'modlock/entities'

const layout = 'file://{resources}/layout/{addon}/board.xml'

/** raised holds the slot of each player who has a board. */
const raised = new Set<number>()

// Raise a board once per player, from a frame, since creating an entity is
// too slow for a client command.
onFrame(() => {
  for (const { player, name, ready, bot } of players()) {
    if (!ready || bot || raised.has(player.slot)) continue
    const pawn = player.pawn()
    if (pawn === undefined) continue
    raised.add(player.slot)
    const board = create('point_clientui_world_panel', {
      position: {
        x: pawn.position.x,
        y: pawn.position.y + 250,
        z: pawn.position.z + 100,
      },
      facing: { pitch: 0, yaw: 0, roll: 0 },
      keys: {
        enabled: true,
        ignore_input: true,
        width: 48,
        height: 24,
        panel_dpi: 32,
        opaque: false,
        orientation: 1,
        dialog_layout_name: layout,
      },
    })
    log('board for', name, board !== undefined)
  }
})
