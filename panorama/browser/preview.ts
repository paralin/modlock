// The preview page draws the screens the mods show the local player, as the
// game would, from the cli.Preview states modlock dev sends on a websocket,
// and sends its presses and chat commands back as that player's input.

import { type Built, Input, Preview, type Stopped } from '../../cli/cli.pb.js'
import { Drawing, type Layouts } from '../src/draw.js'
import { DomPanel, domPanorama } from './dom.js'

/** View is what the page shows, derived from modlock dev's latest state. */
type View =
  | { kind: 'connecting' }
  | { kind: 'gone' }
  | { kind: 'building' }
  | { kind: 'failed'; built: Built; watch: boolean }
  | { kind: 'empty' }
  | { kind: 'showing' }
  | { kind: 'stopped'; stopped: Stopped; watch: boolean }

/** viewOf derives the page's view from one state. */
function viewOf(preview: Preview): View {
  const state = preview.state
  const watch = preview.watch ?? false
  switch (state?.case) {
    case 'failed':
      return { kind: 'failed', built: state.value, watch }
    case 'stopped':
      return { kind: 'stopped', stopped: state.value, watch }
    case 'running':
      return preview.screen?.trees?.length
        ? { kind: 'showing' }
        : { kind: 'empty' }
    default:
      // Building, or the first build has not reported yet.
      return { kind: 'building' }
  }
}

/** labels name each view in the bar. */
const labels: Record<View['kind'], string> = {
  connecting: 'Connecting',
  gone: 'Not running',
  building: 'Building',
  failed: 'Build failed',
  empty: 'Running',
  showing: 'Running',
  stopped: 'Mod stopped',
}

const element = <T extends HTMLElement>(id: string) =>
  document.getElementById(id) as T
const state = element('state')
const command = element<HTMLFormElement>('command')
const note = element('note')
const backdrop = element<HTMLSelectElement>('backdrop')
const backdropFile = element<HTMLInputElement>('backdrop-file')
const stage = element('stage')
const notice = element('notice')

// The stage is one 1920 by 1080 screen, scaled to the window.
const fit = () => {
  const scale = Math.min(innerWidth / 1920, (innerHeight - 36) / 1080)
  stage.style.transform = `scale(${scale})`
}
addEventListener('resize', fit)
fit()

// A mod's layouts load from its latest build, so each build names them anew
// and the drawing loads them again.
let build = 0
const layouts: Layouts = {
  url: (mod, path) =>
    `/layout/${encodeURIComponent(mod)}/${path}?build=${build}`,
  unloaded() {},
}

// The drawing fills the screen, as the game's interface layer does, and a
// press or an edit reaches the mods as the player's.
const root = new DomPanel('Panel', 'ModlockUi')
root.hittest = false
root.style.width = '100%'
root.style.height = '100%'
stage.append(root.element)
const drawing = new Drawing(
  domPanorama,
  root,
  (mod, node) => {
    send({ body: { case: 'press', value: { mod, node } } })
  },
  layouts,
  (mod, node, value) => {
    send({
      body: {
        case: 'change',
        value: { mod, event: { player: 0, node, value } },
      },
    })
  },
)

/** building is true while the latest state reports a build. */
let building = false

/** socket is the open connection to modlock dev, if any. */
let socket: WebSocket | undefined

/** send passes one input to the mods; the state that follows answers it. */
function send(input: Input) {
  if (socket?.readyState === WebSocket.OPEN) {
    socket.send(Input.toJsonString(input))
  }
}

// The bar sends a chat command as the player.
command.addEventListener('submit', (event) => {
  event.preventDefault()
  const line = command.elements.namedItem('line') as HTMLInputElement
  if (line.value.trim()) {
    send({ body: { case: 'command', value: line.value } })
  }
  line.value = ''
})

// The backdrop is none or an image the creator chooses, such as a capture of
// the hideout or a match.
let shown = ''
backdrop.addEventListener('change', () => {
  if (backdrop.value === 'choose') {
    backdrop.value = shown
    backdropFile.click()
    return
  }
  shown = backdrop.value
  stage.style.backgroundImage = shown ? `url(${JSON.stringify(shown)})` : ''
})
backdropFile.addEventListener('change', () => {
  // A chosen image joins the list and becomes the backdrop.
  const file = backdropFile.files?.[0]
  if (!file) return
  const option = new Option(file.name, URL.createObjectURL(file))
  backdrop.add(option, backdrop.options.length - 1)
  backdrop.value = option.value
  backdrop.dispatchEvent(new Event('change'))
  backdropFile.value = ''
})

/** paragraph returns a paragraph of text. */
function paragraph(text: string) {
  const element = document.createElement('p')
  element.textContent = text
  return element
}

/** errors returns a block of error text. */
function errors(text: string) {
  const element = document.createElement('pre')
  element.textContent = text
  return element
}

/** problems describes a failed build's problems, by path in the project. */
function problems(built: Built): string {
  const prefix = built.dir ? `${built.dir}/` : ''
  const lines = (built.diagnostics ?? []).map((problem) => {
    const file = (problem.file ?? '').replace(prefix, '')
    const column = problem.column ? `:${problem.column}` : ''
    const code = problem.code ? ` ${problem.code}` : ''
    return `${file}:${problem.line ?? 0}${column}: ${problem.message ?? ''}${code}`
  })
  return lines.length ? lines.join('\n') : (built.error ?? '')
}

/** render shows view: the bar's state and actions, and the notice. */
function render(view: View) {
  // The bar names the state; commands go only to a running mod.
  state.textContent = labels[view.kind]
  command.hidden = view.kind !== 'empty' && view.kind !== 'showing'
  stage.classList.toggle(
    'dim',
    view.kind === 'building' || view.kind === 'gone',
  )
  const again = (watch: boolean) =>
    watch
      ? 'Fix it and save to try again.'
      : 'Fix it and run modlock dev again.'
  const parts: HTMLElement[] = []
  switch (view.kind) {
    case 'gone':
      parts.push(
        paragraph('modlock dev stopped. Run it again to see your mod.'),
      )
      break
    case 'building':
      parts.push(paragraph('Building'))
      break
    case 'failed':
      parts.push(paragraph('The build failed.'), errors(problems(view.built)))
      parts.push(paragraph(again(view.watch)))
      break
    case 'empty':
      parts.push(
        paragraph('Your mod has not shown a screen to this player yet.'),
      )
      break
    case 'stopped': {
      // The mod's own log usually says why; the host's error follows.
      const { failed, log } = view.stopped
      parts.push(paragraph(`${failed?.mod || 'The mod'} stopped.`))
      if (log?.length) parts.push(errors(log.join('\n')))
      parts.push(errors(failed?.error ?? ''), paragraph(again(view.watch)))
      break
    }
  }
  notice.replaceChildren(...parts)
  notice.hidden = parts.length === 0
}

/**
 * connect follows modlock dev's state, drawing the screen each state carries,
 * and connects again shortly after it stops.
 */
function connect() {
  const next = new WebSocket(`ws://${location.host}/socket`)
  next.onopen = () => {
    socket = next
  }
  next.onmessage = (message) => {
    // A new build names the layouts anew, then the page draws the state.
    const preview = Preview.fromJsonString(String(message.data))
    if (preview.state?.case === 'building' && !building) build++
    building = preview.state?.case === 'building'
    drawing.draw(preview.screen ?? {})
    note.textContent = preview.note ?? ''
    render(viewOf(preview))
  }
  next.onclose = () => {
    socket = undefined
    render({ kind: 'gone' })
    setTimeout(connect, 1000)
  }
}
render({ kind: 'connecting' })
connect()
