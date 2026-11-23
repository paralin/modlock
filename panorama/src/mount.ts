// Every host that draws mod interfaces mounts them in the same place, so a mod
// lays out alike under Modlock's renderer and any other host: one full-screen
// layer inside the game's HUD panel, which lets clicks through to the game.
// Percentages and alignment resolve against the HUD. The layer draws above the
// HUD's stock parts and under the game's tooltips, so a tooltip a mod's panel
// opens shows over it, as do the comms wheel and the load-in fade.

/** TreePanel is the part of a game panel that finds the HUD above it. */
export interface TreePanel {
  readonly paneltype?: string
  GetParent(): TreePanel | null
}

/** MountPanel is the part of a game panel mounting reads and sets. */
export interface MountPanel extends TreePanel {
  hittest: boolean
  readonly style: Record<string, string | null>
  FindChild(id: string): MountPanel | null
  MoveChildBefore(child: MountPanel, before: MountPanel): void
}

/** gameHud returns the game's HUD panel above panel, or the tree's top. */
export function gameHud<P extends TreePanel>(panel: P): P {
  let top = panel
  for (let at: P | null = panel; at; at = at.GetParent() as P | null) {
    if (at.paneltype === 'CitadelHud') return at
    top = at
  }
  return top
}

/**
 * mountInterfaceLayer creates the full-screen pass-through layer named id in
 * the game's HUD above context, where a drawing of mod interfaces goes.
 */
export function mountInterfaceLayer<P extends MountPanel>(
  create: (type: string, parent: P, id: string) => P,
  context: P,
  id: string,
): P {
  // Cover the HUD and let every click through to the game.
  const hud = gameHud(context)
  const layer = create('Panel', hud, id)
  layer.hittest = false
  layer.style.width = '100%'
  layer.style.height = '100%'

  // Draw under the game's tooltips, which a mod's panels open. The HUD holds
  // its TooltipManager as a direct child; a tree without one, such as a
  // context outside the HUD, keeps the layer last.
  const tooltips = hud.FindChild('TooltipManager')
  if (tooltips) hud.MoveChildBefore(layer, tooltips)
  return layer
}
