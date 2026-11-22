// Every host that draws mod interfaces mounts them in the same place, so a mod
// lays out alike under Modlock's renderer and any other host: one full-screen
// layer inside the game's HUD panel, which lets clicks through to the game.
// Percentages and alignment resolve against the HUD, and the layer draws in
// the HUD's own stacking order beside its stock parts.

/** TreePanel is the part of a game panel that finds the HUD above it. */
export interface TreePanel {
  readonly paneltype?: string
  GetParent(): TreePanel | null
}

/** MountPanel is the part of a game panel mounting reads and sets. */
export interface MountPanel extends TreePanel {
  hittest: boolean
  readonly style: Record<string, string | null>
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
  const layer = create('Panel', gameHud(context), id)
  layer.hittest = false
  layer.style.width = '100%'
  layer.style.height = '100%'
  return layer
}
