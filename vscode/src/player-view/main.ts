// The Player View page draws the view the extension posts: the player's
// screen through Modlock's renderer over its DOM stand-in, scaled from the game's
// 1920 by 1080 layout to fit, and the lines they saw and typed. In the sandbox
// it sends the person's presses, edits and commands back as the stand-in
// player's.

import { DomPanel, domPanorama } from '../../../panorama/browser/dom.js'
import { Drawing } from '../../../panorama/src/draw.js'
import { Screen } from '../../../src/wasm/ui/ui.pb.js'

import type { Asked, Shown } from '../player-view.js'
import type { Line } from '../session.js'

/** vscode is the webview's channel to the extension. */
declare function acquireVsCodeApi(): { postMessage(asked: Asked): void }
const vscode = acquireVsCodeApi()

/** element finds one of the page's parts by id. */
function element<T extends HTMLElement>(id: string): T {
  return document.getElementById(id) as T
}

// Find the page's parts.
const viewport = element('viewport')
const stage = element('stage')
const center = element('center')
const announcement = element('announcement')
const empty = element('empty')
const hint = element('hint')
const run = element<HTMLButtonElement>('run')
const lines = element<HTMLOListElement>('lines')
const say = element<HTMLFormElement>('say')
const input = element<HTMLInputElement>('line')
const away = element('away')

// Draw mod trees as panels under the stage, below the messages, and send
// presses and edits while the player is the sandbox's stand-in.
let shown: Shown | undefined
const root = new DomPanel('Panel', 'ModInterfaces')
root.style.width = '100%'
root.style.height = '100%'
stage.prepend(root.element)
const drawing = new Drawing(
  domPanorama,
  root,
  (mod, node) => {
    if (shown?.sandbox) {
      vscode.postMessage({ type: 'press', mod, node })
    }
  },
  undefined,
  (mod, node, value) => {
    if (shown?.sandbox) {
      vscode.postMessage({ type: 'change', mod, node, value })
    }
  },
)

// Scale the stage to fit the page, centered.
new ResizeObserver(() => {
  const scale = Math.min(
    viewport.clientWidth / 1920,
    viewport.clientHeight / 1080,
  )
  const left = (viewport.clientWidth - 1920 * scale) / 2
  const top = (viewport.clientHeight - 1080 * scale) / 2
  stage.style.transform = `translate(${left}px, ${top}px) scale(${scale})`
}).observe(viewport)

// Send typed commands, keeping them for Up and Down.
const history: string[] = []
let recalled = 0
say.addEventListener('submit', (event) => {
  // Send the typed line unless it is blank.
  event.preventDefault()
  const line = input.value.trim()
  if (line === '') {
    return
  }
  vscode.postMessage({ type: 'command', line })

  // Keep it in the recall history and clear the input.
  if (history.at(-1) !== line) {
    history.push(line)
  }
  recalled = history.length
  input.value = ''
})
input.addEventListener('keydown', (key) => {
  // The arrow keys step through the recall history.
  const step = { ArrowUp: -1, ArrowDown: 1 }[key.key]
  if (step === undefined || history.length === 0) {
    return
  }
  key.preventDefault()
  recalled = Math.max(0, Math.min(history.length, recalled + step))
  input.value = history[recalled] ?? ''
})
run.addEventListener('click', () => vscode.postMessage({ type: 'run' }))

// Draw each view the extension posts, then ask for the current one.
let last = 0
let linesOf = ''
window.addEventListener('message', (message: MessageEvent<Shown>) => {
  // Draw the screen, and say why it is empty.
  shown = message.data
  const screen = Screen.fromJson(shown.screen)
  drawing.draw(screen)
  document.body.classList.toggle('playable', shown.sandbox)
  const blank = !screen.trees?.length
  empty.hidden = !blank
  run.hidden = shown.running
  hint.textContent = !shown.running
    ? 'Run your mod to see what a player sees.'
    : !shown.player
      ? 'Waiting for a player to join.'
      : shown.sandbox
        ? 'Nothing on screen yet. Type a command below to try your mod; what it shows the player appears here.'
        : `Nothing on screen yet. What your mod shows ${shown.player} appears here.`

  // Offer the command line only to the sandbox's stand-in.
  say.hidden = !shown.sandbox
  away.hidden = shown.sandbox || !shown.player

  // Add new lines, starting over for another player or session.
  if (shown.lines.length === 0 || shown.player !== linesOf) {
    lines.replaceChildren()
    last = 0
    linesOf = shown.player
  }
  for (const line of shown.lines) {
    if (line.id > last) {
      add(line, line.id === shown.lines.at(-1)?.id)
      last = line.id
    }
  }
})
vscode.postMessage({ type: 'ready' })

/** where names where the player saw a line, after the mod that showed it. */
const where: Record<Line['kind'], string> = {
  chat: '',
  center: 'mid-screen',
  announcement: 'announcement',
  command: '',
}

/**
 * add appends line to the transcript and, when it is the newest, shows a
 * center text or announcement on the screen as the game does.
 */
function add(line: Line, newest: boolean): void {
  // Build the line's item: who sent it, where it shows and its text.
  const item = document.createElement('li')
  item.className = line.kind
  const from = document.createElement('span')
  from.className = 'from'
  from.textContent = line.mod || shown?.player || 'Player'
  item.append(from)
  if (where[line.kind]) {
    const kind = document.createElement('span')
    kind.className = 'where'
    kind.textContent = where[line.kind]
    item.append(kind)
  }
  item.append(line.title ? `${line.title}: ${line.text}` : line.text)

  // Keep the newest line in sight.
  lines.append(item)
  item.scrollIntoView({ block: 'end' })

  // Show it on the screen for a moment.
  if (!newest || (line.kind !== 'center' && line.kind !== 'announcement')) {
    return
  }
  const shownOn = line.kind === 'center' ? center : announcement
  const title = document.createElement('strong')
  title.textContent = line.title
  shownOn.replaceChildren(...(line.title ? [title] : []), line.text)
  shownOn.classList.remove('shown')
  void shownOn.offsetWidth
  shownOn.classList.add('shown')
}
