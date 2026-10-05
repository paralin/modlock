// The Modlock renderer draws the interfaces mods show the local player. Its
// layout holds the relay panel; this script draws each mod's tree with stock
// panels over the game's HUD, which stays on screen in every match. A press
// runs the console command that sends it to the server.

import { Bridge } from './bridge.js'
import { Drawing, pressCommand } from './draw.js'

/** relay is the origin of the relay page, as the controller's default. */
const relay = 'https://hyperline.gg'

/** hud returns the game's HUD above panel, or the layout's own root. */
function hud(panel: Panel): Panel {
  for (let at: Panel | null = panel; at; at = at.GetParent()) {
    if (at.paneltype === 'CitadelHud') {
      return at
    }
  }
  return panel
}

const context = $.GetContextPanel()
const root = $.CreatePanel('Panel', hud(context), 'ModlockUi')
root.hittest = false
root.style.width = '100%'
root.style.height = '100%'
const drawing = new Drawing($, root, (mod, node) => $.DispatchEvent('CitadelConCommand', pressCommand(mod, node)))
new Bridge(context.FindChildTraverse('ModlockUiState')!, `${relay}/app/relay/bridge.html`, (screen) =>
  drawing.draw(screen),
)
