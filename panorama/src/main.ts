// The Modlock renderer draws the interfaces mods show the local player. Its
// layout holds the relay panel; this script draws each mod's tree with stock
// panels over the game's HUD, which stays on screen in every match, and hides
// the parts of that HUD the trees' roots list. A press runs the console
// command that sends it to the server.

import { Bridge } from './bridge.js'
import { Drawing, pressCommand } from './draw.js'
import { Hud } from './hud.js'

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
  `${relay}/app/relay/bridge.html`,
  (screen) => {
    drawing.draw(screen)
    stock.draw(screen)
  },
)

// A temporary probe: log which key bind names the mouse wheel answers to.
for (const key of [
  'key_MWHEELUP',
  'key_MWHEELDOWN',
  'MWHEELUP',
  'MWHEELDOWN',
  'key_mwheelup',
  'key_mwheeldown',
  'mwheelup',
  'mwheeldown',
  'key_MOUSE_WHEEL_UP',
  'key_MOUSE_WHEEL_DOWN',
  'mouse_wheel_up',
  'mouse_wheel_down',
]) {
  try {
    $.RegisterKeyBind(context, key, () => $.Msg(`[Modlock probe] ${key}`))
    $.Msg(`[Modlock probe] bound ${key}`)
  } catch (error) {
    $.Msg(`[Modlock probe] refused ${key}: ${String(error)}`)
  }
}
