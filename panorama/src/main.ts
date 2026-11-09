// The Modlock renderer draws the interfaces mods show the local player. Its
// layout holds the relay panel; this script draws each mod's tree with stock
// panels over the game's HUD, which stays on screen in every match, and its
// stock guard hides the parts of that HUD the trees' roots list while the
// screen is live, freeing the cursor while one asks. A press runs the console
// command that sends it to the server.

import { Bridge } from './bridge.js'
import { AddonLayouts, Drawing, inputCommand, pressCommand } from './draw.js'
import { StockGuard } from './guard.js'
import { Cursor, cursorAsks, cursorFreed, hiddenParts } from './hud.js'

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
// The drawing reaches the game through $, and plays sounds through the game's
// global sound call. Presses and edits go to the host as console commands, and
// each mod's layouts load from the content addon its tree names.
const layouts = new AddonLayouts()
const drawing = new Drawing(
  {
    CreatePanel: (...args: Parameters<typeof $.CreatePanel>) =>
      $.CreatePanel(...args),
    DispatchEvent: (event: string, ...values: unknown[]) =>
      $.DispatchEvent(event, ...values),
    Msg: (...values: unknown[]) => $.Msg(...values),
    MousePosition: () => $.MousePosition(),
    RegisterEventHandler: (event, panel, handler) =>
      $.RegisterEventHandler(event, panel as Panel, handler),
    PlaySound: (sound: string) => {
      PlayUISoundEvent(sound)
    },
    Schedule: (seconds: number, callback: () => void) => {
      $.Schedule(seconds, callback)
    },
    Localize: (token: string, panel: object) => $.Localize(token, panel),
  },
  root,
  (mod, node) => $.DispatchEvent('CitadelConCommand', pressCommand(mod, node)),
  layouts,
  (mod, node, value) =>
    $.DispatchEvent('CitadelConCommand', inputCommand(mod, node, value)),
)
const stock = new StockGuard(
  game,
  {
    Schedule: (seconds, callback) => {
      $.Schedule(seconds, callback)
    },
    now: () => Date.now() / 1000,
  },
  // Hiding the whole HUD leaves this layout, which reads the screens.
  [context],
)
// The game says the player is leaving as they ask to, and as it shuts the
// map down; a server that just goes away says neither, and the guard's lease
// runs out instead. Both events go on to other handlers, and close the
// tooltips and windows the mods' panels opened.
for (const event of ['CitadelDisconnectFromGame', 'GameSystemGameShutdown']) {
  $.RegisterForUnhandledEvent(event, () => {
    stock.leave()
    drawing.untipAll()
    return false
  })
}
const cursor = new Cursor((event, ...values) => {
  $.DispatchEvent(event, ...values)
}, game)
new Bridge(
  context.FindChildTraverse('ModlockUiState')!,
  // The session's boot script names the page for its relay and port.
  context.GetAttributeString('modlock_bridge', defaultBridge),
  (screen) => {
    layouts.follow(screen)
    drawing.draw(screen)
    stock.screen(screen ? hiddenParts(screen) : null)
    cursor.set(cursorFreed(screen), cursorAsks(screen))
  },
  () => stock.alive(),
)
