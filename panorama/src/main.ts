// The Modlock renderer draws the interfaces mods show the local player. Its
// layout holds the relay panel; this script draws each mod's tree with stock
// panels over the game's HUD, which stays on screen in every match, and its
// stock guard hides the parts of that HUD the trees' roots list while the
// screen is live, freeing the cursor while one asks. A press runs the console
// command that sends it to the server.

import { GameAction } from '../../src/wasm/ui/ui.pb.js'
import { gameActionCommand, GameActions } from './actions.js'
import { Bridge } from './bridge.js'
import {
  AddonLayouts,
  CHAT_OPEN,
  Drawing,
  inputCommand,
  pressCommand,
} from './draw.js'
import { StockGuard } from './guard.js'
import { gameHud, mountInterfaceLayer } from './mount.js'
import {
  abilityBarMods,
  abilityRows,
  Cursor,
  cursorAsks,
  cursorFreed,
  gameActionMods,
  hiddenParts,
} from './hud.js'
import type { MenuHost } from './menu.js'

/**
 * defaultBridge is the relay page a package without the session's boot script
 * reads, which serves the screen on the default port.
 */
const defaultBridge = 'https://hyperline.gg/app/relay/bridge.html'

const context = $.GetContextPanel()
const game = gameHud(context)
const root = mountInterfaceLayer(
  (type, parent, id) => $.CreatePanel(type, parent, id),
  context,
  'ModlockUi',
)
// The drawing reaches the game through $, and plays sounds through the game's
// global sound call. Presses and edits go to the host as console commands, and
// each mod's layouts load from the content addon its tree names.
const layouts = new AddonLayouts()
const cursor = new Cursor((event, ...values) => {
  $.DispatchEvent(event, ...values)
}, game)
// menuHost tells the menus and the game actions when a stock overlay has the
// player's input.
const menuHost: MenuHost = {
  allowed: () => stock.allowed(),
  stock: () => {
    // Chat, the shop and stock popups keep their input when already open.
    const chat = game.FindChildTraverse('Chat')
    if (
      chat?.BHasClass(CHAT_OPEN) ||
      chat?.FindChildTraverse('ChatInput')?.BHasKeyFocus?.() ||
      game.FindChildTraverse('CitadelHudHeroShop')?.BHasClass('gShopOpen') ||
      game.BHasClass('gEditingBuilds') ||
      game.BHasClass('gItemDraftOpen')
    )
      return 'other'
    const manager = game.FindChild('PopupManager')
    if (manager) {
      for (let i = 0; i < manager.GetChildCount(); i++) {
        const panel = manager.GetChild(i)
        if (panel?.BHasClass('PopupPanel') && !panel.BHasClass('Hidden'))
          return 'other'
      }
    }
    if (
      game.BHasClass('ShowEscapeMenu') ||
      game.FindChild('EscapeMenu')?.BHasClass('ShowEscapeMenu')
    )
      return 'escape'
    return null
  },
  capture: (open) => cursor.menuCapture(open),
  resume: () => {
    $.DispatchEvent('CitadelResumePlaying', game)
  },
}
const actions = new GameActions(
  (mod, action, phase) =>
    $.DispatchEvent('CitadelConCommand', gameActionCommand(mod, action, phase)),
  menuHost,
  (seconds, callback) => {
    $.Schedule(seconds, callback)
  },
)
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
    PlaySound: (sound: string) => PlayUISoundEvent(sound),
    StopSound: (playing: number) => {
      StopUISoundEvent(playing)
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
  menuHost,
)
const stock = new StockGuard(
  game,
  {
    Schedule: (seconds, callback) => {
      $.Schedule(seconds, callback)
    },
    now: () => Date.now() / 1000,
    listen: (panel, handler) => {
      $.RegisterEventHandler('Activated', panel as Panel, () => {
        handler()
      })
    },
    press: (mod, node) =>
      $.DispatchEvent('CitadelConCommand', pressCommand(mod, node)),
    create: (type, parent, id, properties = {}) =>
      $.CreatePanel(type, parent as Panel, id, properties),
    localize: (token, panel) => $.Localize(token, panel),
    dispatch: (event, ...values) => {
      $.DispatchEvent(event, ...values)
    },
  },
  // Hiding the whole HUD leaves this layout, which reads the screens.
  [context],
)
// The game says the player is leaving as they ask to, and as it shuts the
// map down; a server that just goes away says neither, and the guard's lease
// runs out instead. Both events go on to other handlers, and close the
// tooltips and windows the mods' panels opened and the sounds they started.
for (const event of ['CitadelDisconnectFromGame', 'GameSystemGameShutdown']) {
  $.RegisterForUnhandledEvent(event, () => {
    // End every press and give back what the screen held.
    stock.leave()
    actions.cancel()
    drawing.release()
    cursor.set(false, 0)
    return false
  })
}
$.RegisterForUnhandledEvent('CitadelScoreboardToggle', (pressed: boolean) => {
  drawing.scoreboard(pressed)
  actions.change(GameAction.SCOREBOARD, pressed)
  return false
})
new Bridge(
  context.FindChildTraverse('ModlockUiState')!,
  // The session's boot script names the page for its relay and port.
  context.GetAttributeString('modlock_bridge', defaultBridge),
  (screen) => {
    // Each part of the renderer follows what the new screen asks of it.
    layouts.follow(screen)
    actions.follow(gameActionMods(screen))
    stock.screen(
      screen ? hiddenParts(screen) : null,
      abilityBarMods(screen),
      abilityRows(screen),
    )
    cursor.set(cursorFreed(screen), cursorAsks(screen))
    drawing.draw(screen)
  },
  () => stock.alive(),
)
