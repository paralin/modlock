// The Modlock renderer draws the interfaces mods show the local player. Its
// layout holds the relay panel; this script draws each mod's tree with stock
// panels over the game's HUD, which stays on screen in every match, and hides
// the parts of that HUD the trees' roots list. A press runs the console
// command that sends it to the server.

import { Bridge } from './bridge.js'
import { Drawing, pressCommand } from './draw.js'
import { Hud, hiddenParts } from './hud.js'

/**
 * defaultBridge is the relay page a package without the session's boot script
 * reads, which serves the screen on the default port.
 */
const defaultBridge = 'https://hyperline.gg/app/relay/bridge.html'

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
const game = hud(context)
const root = $.CreatePanel('Panel', game, 'ModlockUi')
root.hittest = false
root.style.width = '100%'
root.style.height = '100%'
const drawing = new Drawing($, root, (mod, node) =>
  $.DispatchEvent('CitadelConCommand', pressCommand(mod, node)),
)
const stock = new Hud(game, (seconds, callback) => {
  $.Schedule(seconds, callback)
})
new Bridge(
  context.FindChildTraverse('ModlockUiState')!,
  // The session's boot script names the page for its relay and port.
  context.GetAttributeString('modlock_bridge', defaultBridge),
  (screen) => {
    drawing.draw(screen)
    stock.draw(hiddenParts(screen))
  },
)
