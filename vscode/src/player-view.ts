import { randomBytes } from 'node:crypto'
import * as vscode from 'vscode'

import type { JsonValue } from '@aptre/protobuf-es-lite'
import panels from '../../panorama/browser/panels.css' with { type: 'text' }
import type { InputValue } from '../../panorama/src/draw.js'
import { Screen } from '../../src/wasm/ui/ui.pb.js'

import type { Line, Session } from './session.js'

/** Shown is what the page draws: one player's screen and transcript. */
export interface Shown {
  /** running is true while a session runs. */
  running: boolean
  /** sandbox is true when the player is the sandbox's stand-in. */
  sandbox: boolean
  /** player names whose view it is, empty when no player has joined. */
  player: string
  /** screen is the player's Screen as protobuf JSON. */
  screen: JsonValue
  /** lines are the player's latest messages and commands. */
  lines: Line[]
}

/** Asked is what the page asks the extension to do. */
export type Asked =
  | { type: 'ready' }
  | { type: 'run' }
  | { type: 'command'; line: string }
  | { type: 'press'; mod: string; node: string }
  | { type: 'change'; mod: string; node: string; value: InputValue }

/** Player acts as the stand-in player in the sandbox. */
export interface Player {
  /** command sends line as a command the player typed. */
  command(line: string): void
  /** press presses node in the interface mod shows the player in slot. */
  press(slot: number, mod: string, node: string): void
  /** change sets node, an input mod shows the player in slot, to value. */
  change(slot: number, mod: string, node: string, value: InputValue): void
}

/**
 * PlayerView shows what one player sees in a webview: each mod's interface,
 * drawn by Modlock's own renderer over the page's DOM, and the messages mods
 * show them. It follows the chosen player, or the first player in the
 * session. In the sandbox the person plays the stand-in player from it,
 * pressing buttons, editing inputs and typing commands.
 */
export class PlayerView {
  private panel: vscode.WebviewPanel | undefined
  /** slot is the chosen player's slot; undefined follows the first player. */
  private slot: number | undefined

  constructor(
    private readonly extension: vscode.Uri,
    private readonly session: Session,
    private readonly player: Player,
  ) {
    session.onDidChange(() => this.post())
    session.onDidChangeSeen((slot) => {
      if (slot === this.shown()) {
        this.post()
      }
    })
  }

  /** show opens the view on the player in slot, or the first player. */
  show(slot?: number): void {
    // Reveal an open panel on the asked-for player.
    this.slot = slot
    if (this.panel) {
      this.panel.reveal(undefined, true)
      this.post()
      return
    }

    // Open the page; it asks for the view once it has loaded.
    const dist = vscode.Uri.joinPath(this.extension, 'dist')
    this.panel = vscode.window.createWebviewPanel(
      'modlock.playerView',
      'Player View',
      { viewColumn: vscode.ViewColumn.Beside, preserveFocus: true },
      { enableScripts: true, localResourceRoots: [dist] },
    )
    this.panel.iconPath = vscode.Uri.joinPath(
      this.extension,
      'media',
      'modlock.svg',
    )
    this.panel.webview.html = page(
      this.panel.webview,
      vscode.Uri.joinPath(dist, 'player-view.js'),
    )
    this.panel.webview.onDidReceiveMessage((asked: Asked) => this.act(asked))
    this.panel.onDidDispose(() => {
      this.panel = undefined
    })
  }

  /** act does what the page asked. */
  private act(asked: Asked): void {
    const slot = this.shown()
    switch (asked.type) {
      case 'ready':
        this.post()
        break
      case 'run':
        void vscode.commands.executeCommand('modlock.run')
        break
      case 'command':
        this.player.command(asked.line)
        break
      case 'press':
        if (slot !== undefined) {
          this.player.press(slot, asked.mod, asked.node)
        }
        break
      case 'change':
        if (slot !== undefined) {
          this.player.change(slot, asked.mod, asked.node, asked.value)
        }
        break
    }
  }

  /** shown returns the slot of the player the view draws. */
  private shown(): number | undefined {
    if (this.slot !== undefined && this.session.players.has(this.slot)) {
      return this.slot
    }
    return [...this.session.players.keys()].sort((a, b) => a - b)[0]
  }

  /** post sends the shown player's view to the page. */
  private post(): void {
    // Send the shown player's screen and transcript to an open panel.
    if (!this.panel) {
      return
    }
    const slot = this.shown()
    const player =
      slot === undefined ? undefined : this.session.players.get(slot)
    const name = player ? player.name || `Player ${player.slot}` : ''
    const shown: Shown = {
      running: this.session.running,
      sandbox: this.session.sandbox !== '',
      player: name,
      screen: Screen.toJson(
        slot === undefined ? {} : this.session.screen(slot),
      ),
      lines: slot === undefined ? [] : this.session.transcript(slot),
    }
    this.panel.title = name ? `Player View: ${name}` : 'Player View'
    void this.panel.webview.postMessage(shown)
  }
}

/** page is the view's HTML, which runs only the bundled page script. */
function page(webview: vscode.Webview, script: vscode.Uri): string {
  const nonce = randomBytes(16).toString('base64')
  return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'nonce-${nonce}'; script-src 'nonce-${nonce}';">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style nonce="${nonce}">${css}</style>
</head>
<body>
<main id="viewport" aria-label="Screen">
<div id="stage"><div id="center" aria-live="polite"></div><div id="announcement" aria-live="polite"></div></div>
<div id="empty"><p id="hint"></p><button id="run" type="button" hidden>Run Mod</button></div>
</main>
<section id="chat" aria-label="Chat">
<ol id="lines"></ol>
<form id="say" hidden><input id="line" aria-label="Command" autocomplete="off" placeholder="Type a command, such as /hello"><button type="submit">Send</button></form>
<p id="away" hidden>The player types commands in the game's chat.</p>
</section>
<script nonce="${nonce}" src="${webview.asWebviewUri(script).toString()}"></script>
</body>
</html>`
}

/**
 * css lays out the page: the game's 1920 by 1080 screen scaled to fit above
 * the chat. Within the screen it lays out the drawn panels the way the game's
 * panels lay out: a panel stacks its children unless it flows them down or
 * right, alignment places a child along the axes its parent does not flow,
 * and a fill length shares the space left in the parent's flow, as Modlock's
 * panels.css approximates. The page itself takes VS Code's theme.
 */
const css = `
html, body { height: 100%; margin: 0; }
body { display: flex; flex-direction: column; overflow: hidden; background: var(--vscode-editor-background); color: var(--vscode-foreground); font-family: var(--vscode-font-family); font-size: var(--vscode-font-size); }
button { min-height: 26px; padding: 2px 12px; border: 1px solid var(--vscode-button-border, transparent); border-radius: 2px; background: var(--vscode-button-background); color: var(--vscode-button-foreground); font: inherit; cursor: pointer; }
button:hover { background: var(--vscode-button-hoverBackground); }
:focus-visible { outline: 1px solid var(--vscode-focusBorder); outline-offset: 2px; }

#viewport { position: relative; flex: 1 1 auto; min-height: 120px; overflow: hidden; background: var(--vscode-sideBar-background, var(--vscode-editor-background)); }
#stage { position: absolute; left: 0; top: 0; width: 1920px; height: 1080px; transform-origin: 0 0; background: #2a3138; box-shadow: 0 8px 32px rgb(0 0 0 / 0.45); overflow: hidden; }
#empty { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 12px; padding: 16px; text-align: center; pointer-events: none; }
#empty[hidden] { display: none; }
#empty > * { pointer-events: auto; }
#hint { max-width: 40ch; margin: 0; line-height: 1.5; color: var(--vscode-descriptionForeground); }

#center, #announcement { position: absolute; left: 0; right: 0; z-index: 1; text-align: center; color: #fff; text-shadow: 0 2px 6px rgb(0 0 0 / 0.8); pointer-events: none; }
#center { top: 30%; font-size: 44px; font-weight: 600; }
#announcement { top: 12%; font-size: 34px; }
#announcement strong { display: block; font-size: 56px; letter-spacing: -0.02em; }
.shown { animation: shown 5s ease-out forwards; }
@keyframes shown { 0%, 80% { opacity: 1; } 100% { opacity: 0; } }
@media (prefers-reduced-motion: reduce) { .shown { animation: none; } }

#chat { flex: 0 0 auto; display: flex; flex-direction: column; max-height: 40%; border-top: 1px solid var(--vscode-panel-border, var(--vscode-widget-border, transparent)); }
#lines { flex: 1 1 auto; margin: 0; padding: 6px 12px; overflow-y: auto; list-style: none; line-height: 1.5; }
#lines:empty { display: none; }
#lines .from { margin-right: 8px; color: var(--vscode-textLink-foreground); font-weight: 600; }
#lines .where { margin-right: 8px; color: var(--vscode-descriptionForeground); }
#lines .command { color: var(--vscode-descriptionForeground); }
#say { display: flex; gap: 6px; padding: 8px 12px; }
#say[hidden], #away[hidden] { display: none; }
#line { flex: 1 1 auto; min-width: 0; min-height: 24px; padding: 2px 6px; border: 1px solid var(--vscode-input-border, transparent); border-radius: 2px; background: var(--vscode-input-background); color: var(--vscode-input-foreground); font: inherit; }
#line::placeholder { color: var(--vscode-input-placeholderForeground); }
#away { margin: 0; padding: 8px 12px; color: var(--vscode-descriptionForeground); }

#stage .p-Label { color: #fff; font-family: sans-serif; font-size: 16px; }
.playable .p-Button:hover { filter: brightness(1.25); }
.playable .p-Button:active { filter: brightness(0.85); }
.p-Button:focus-visible { outline: 3px solid var(--vscode-focusBorder); }
${panels}
`
