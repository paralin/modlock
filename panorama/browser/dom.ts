import type { Panel, Panorama } from '../src/draw.js'
import { cascade, parseSheet, type Rule, type Scope } from './sheets.js'

/**
 * DomPanel stands in for a Panorama panel with one HTML element, so a mod's
 * interface renders in a browser for layout work. It approximates Panorama
 * layout with the CSS in panels.css, which the page must link: children
 * overlap unless the panel flows them down or right, and fill-parent-flow
 * takes the remaining space. Fonts and engine-only styles are approximate.
 * A layout it loads draws its panels, their classes and inline styles, and
 * applies its style sheets to the panel it loads into and that panel's
 * subtree, but runs none of its scripts; the installed game remains the
 * reference.
 */
export class DomPanel implements Panel {
  readonly element: HTMLElement
  readonly style: Record<string, string>
  /** sheeted holds the values the style sheets gave this panel, by name. */
  private readonly sheeted = new Map<string, string>()
  private readonly handlers = new Map<string, (...args: unknown[]) => void>()
  private readonly attributes = new Map<string, string>()
  /** inline holds the styles code and the layout set, by camel-case name. */
  private readonly inline: Record<string, string>
  private valid = true
  private accepts = false
  /** options holds a dropdown's option panels by id. */
  private readonly options = new Map<string, DomPanel>()
  private dialog: Record<string, string> = {}

  constructor(
    readonly paneltype: string,
    readonly id: string,
    host?: HTMLElement,
  ) {
    // Mount the element with its panel classes and a back reference.
    this.element = host ?? document.createElement('div')
    this.element.classList.add('p', `p-${paneltype}`, 'flow-none')
    if (id) this.element.dataset.id = id
    ;(this.element as HTMLElement & { panel?: DomPanel }).panel = this

    // Engine style writes arrive one property at a time and win over the
    // style sheets; clearing one, by null or a delete, gives the sheets'
    // value back.
    const element = this.element
    const sheeted = (name: string) =>
      this.sheeted.get(name.replace(/[A-Z]/g, (l) => `-${l.toLowerCase()}`)) ??
      ''
    this.inline = {}
    this.style = new Proxy<Record<string, string>>(this.inline, {
      set(target, name: string, value: string | null) {
        if (value === null) {
          delete target[name]
          applyStyle(element, name, sheeted(name))
          return true
        }
        target[name] = value
        applyStyle(element, name, value)
        return true
      },
      deleteProperty(target, name: string) {
        delete target[name]
        applyStyle(element, name, sheeted(name))
        return true
      },
    })

    // Native activation, hover and focus events map to their DOM equivalents.
    this.element.addEventListener('click', (event) => {
      if (!this.enabled) return
      event.stopPropagation()
      if (paneltype === 'ToggleButton') this.checked = !this.checked
      this.handlers.get('onactivate')?.()
    })

    // A form element's edits arrive as the event its panel type raises.
    this.element.addEventListener('input', () =>
      this.handlers.get(
        paneltype === 'Slider' ? 'onvaluechanged' : 'ontextentrychange',
      )?.(),
    )
    this.element.addEventListener('change', () =>
      this.handlers.get('oninputsubmit')?.(),
    )
    this.element.addEventListener('mouseenter', () =>
      this.handlers.get('onmouseover')?.(),
    )
    this.element.addEventListener('mouseleave', () =>
      this.handlers.get('onmouseout')?.(),
    )
    this.element.addEventListener('focus', () =>
      this.handlers.get('onfocus')?.(),
    )
    this.element.addEventListener('blur', () => this.handlers.get('onblur')?.())
  }

  get text(): string {
    return this.element instanceof HTMLInputElement
      ? this.element.value
      : (this.element.textContent ?? '')
  }
  set text(value: string) {
    if (this.element instanceof HTMLInputElement) {
      this.element.value = value
      return
    }
    this.element.textContent = value
    shrink(this.element)
  }

  get value(): number {
    return this.element instanceof HTMLInputElement
      ? Number(this.element.value)
      : 0
  }
  set value(value: number) {
    if (this.element instanceof HTMLInputElement) {
      this.element.value = String(value)
    }
  }

  get checked(): boolean {
    return this.element.getAttribute('aria-pressed') === 'true'
  }
  set checked(value: boolean) {
    this.element.setAttribute('aria-pressed', String(value))
  }

  /** SetDialogVariable keeps a value for Localize to fill a token with. */
  SetDialogVariable(name: string, value: string) {
    this.dialog[name] = value
  }

  /** dialogVariables returns the values SetDialogVariable kept. */
  dialogVariables(): Readonly<Record<string, string>> {
    return this.dialog
  }

  /** AddOption makes a label panel one of a dropdown's options. */
  AddOption(option: DomPanel) {
    if (!(this.element instanceof HTMLSelectElement)) return
    option.element.remove()
    this.options.set(option.id, option)
    this.element.append(new Option(option.text, option.id))
  }

  SetSelected(id: string) {
    if (this.element instanceof HTMLSelectElement) this.element.value = id
  }

  GetSelected(): DomPanel | null {
    return this.element instanceof HTMLSelectElement
      ? (this.options.get(this.element.value) ?? null)
      : null
  }

  get visible(): boolean {
    return !this.element.classList.contains('hidden')
  }
  set visible(value: boolean) {
    this.element.classList.toggle('hidden', !value)
  }

  get hittest(): boolean {
    return !this.element.classList.contains('no-hit')
  }
  set hittest(value: boolean) {
    this.element.classList.toggle('no-hit', !value)
  }

  get hittestchildren(): boolean {
    return !this.element.classList.contains('no-hit-children')
  }
  set hittestchildren(value: boolean) {
    this.element.classList.toggle('no-hit-children', !value)
  }

  get enabled(): boolean {
    return !this.element.classList.contains('disabled')
  }
  set enabled(value: boolean) {
    this.element.classList.toggle('disabled', !value)
  }

  IsValid() {
    return this.valid
  }

  DeleteAsync() {
    this.valid = false
    this.element.remove()
  }

  SetAcceptsFocus(accepts: boolean) {
    this.accepts = accepts
    if (accepts) this.element.tabIndex = 0
    else this.element.removeAttribute('tabindex')
  }

  SetFocus() {
    if (!this.accepts) return false
    this.element.focus()
    return true
  }

  BHasKeyFocus() {
    return document.activeElement === this.element
  }

  GetParent(): DomPanel | null {
    return panelOf(this.element.parentElement)
  }

  GetChildCount() {
    return this.element.children.length
  }

  GetChild(index: number): DomPanel | null {
    return panelOf(this.element.children[index])
  }

  FindChildTraverse(id: string): DomPanel | null {
    return panelOf(this.element.querySelector(`[data-id="${CSS.escape(id)}"]`))
  }

  ScrollToBottom() {
    this.element.scrollTop = this.element.scrollHeight
  }

  SetParent(parent: DomPanel) {
    parent.element.append(this.element)
  }

  MoveChildBefore(child: DomPanel, before: DomPanel) {
    this.element.insertBefore(child.element, before.element)
  }

  MoveChildAfter(child: DomPanel, after: DomPanel) {
    after.element.after(child.element)
  }

  AddClass(name: string) {
    this.element.classList.add(name)
  }

  RemoveClass(name: string) {
    this.element.classList.remove(name)
  }

  SetPanelEvent(event: string, handler: (...args: unknown[]) => void) {
    this.handlers.set(event, handler)
  }

  ClearPropertyFromCode(name: string) {
    delete this.style[camel(name)]
  }

  SetAttributeString(name: string, value: string) {
    this.attributes.set(name, value)
  }

  GetAttributeString(name: string, fallback: string) {
    return this.attributes.get(name) ?? fallback
  }

  SetImage(path: string) {
    this.element.style.backgroundImage = path
      ? `url(${JSON.stringify(path)})`
      : ''
  }

  /**
   * BLoadLayout loads the layout a page path such as /layout/mod/board.xml
   * serves into this panel, as Panorama does: the layout's top panel
   * describes this one and its children become this panel's, though a style
   * the drawing already gave this panel stays. It loads in the background,
   * and refuses a game path, which a browser cannot read.
   */
  BLoadLayout(url: string) {
    if (!url.startsWith('/')) return false
    void fetchOnce(url, (text) =>
      new DOMParser().parseFromString(text, 'text/xml'),
    )
      .then((layout) => {
        const top = [...layout.documentElement.children].find(
          (child) => !nonPanels.has(child.tagName.toLowerCase()),
        )
        if (!this.valid) return
        for (const child of layout.documentElement.children)
          if (child.tagName.toLowerCase() === 'styles')
            this.loadSheets(child, url)
        if (top) this.loadElement(top, new Set(Object.keys(this.inline)))
      })
      .catch((error: unknown) => console.debug(`layout ${url}:`, error))
    return true
  }

  // loadElement gives this panel a layout element's attributes, except the
  // style properties in kept, then adds a panel for each of its child
  // elements.
  private loadElement(source: Element, kept = new Set<string>()) {
    for (const { name, value } of source.attributes) {
      switch (name) {
        case 'class':
          for (const each of value.split(/\s+/)) if (each) this.AddClass(each)
          break
        case 'style':
          for (const declaration of value.split(';')) {
            const [property = '', ...rest] = declaration.split(':')
            const key = camel(property.trim())
            if (key && !kept.has(key)) this.style[key] = rest.join(':').trim()
          }
          break
        case 'text':
          this.text = value
          break
        case 'src':
          this.SetImage(value)
          break
        case 'hittest':
        case 'visible':
        case 'enabled':
          this[name] = value !== 'false'
          break
      }
    }
    for (const child of source.children) {
      if (nonPanels.has(child.tagName.toLowerCase())) continue
      const panel = new DomPanel(child.tagName, child.getAttribute('id') ?? '')
      panel.SetParent(this)
      panel.loadElement(child)
    }
  }

  // loadSheets styles this panel's subtree with each sheet a layout's styles
  // element includes, fetched from the page beside the layout at url.
  private loadSheets(styles: Element, url: string) {
    const build = new URL(url, location.href).search
    for (const include of styles.children) {
      const path = sheetPath(include.getAttribute('src') ?? '')
      if (!path) continue
      void fetchSheet(`/styles/${path}${build}`).then((rules) => {
        const { element } = this
        if (!this.valid) return
        if (!scopes.some((s) => s.element === element && s.rules === rules))
          scopes.push({ element, rules })
        restyle()
      })
    }
  }

  /** applySheets writes the values the style sheets now give this panel. */
  applySheets(values: ReadonlyMap<string, string>) {
    for (const [name, value] of values) {
      if (this.sheeted.get(name) === value) continue
      this.sheeted.set(name, value)
      if (!(camel(name) in this.inline)) applyStyle(this.element, name, value)
    }
    for (const name of this.sheeted.keys()) {
      if (values.has(name)) continue
      this.sheeted.delete(name)
      if (!(camel(name) in this.inline)) applyStyle(this.element, name, '')
    }
  }
}

// sheetPath returns the page path, <addon>/<path>.css, of a sheet a layout
// includes by its game path, or '' for one the page does not serve.
function sheetPath(src: string): string {
  const match =
    /^(?:s2r:\/\/panorama|file:\/\/\{resources\})\/styles\/([^/]+\/.+)\.v?css$/.exec(
      src,
    )
  return match ? `${match[1]}.css` : ''
}

// fetched holds each page path's parsed answer, so every panel that loads
// the same layout or sheet shares one fetch, and a panel loaded again, such as
// a screen drawn anew, gets it before the browser paints. A path carries its
// build, so a new build fetches anew; a failed fetch leaves the cache, so a
// later load tries again.
const fetched = new Map<string, Promise<unknown>>()
function fetchOnce<T>(path: string, parse: (text: string) => T): Promise<T> {
  let answer = fetched.get(path) as Promise<T> | undefined
  if (!answer) {
    answer = fetch(path).then(async (response) => {
      if (!response.ok) throw new Error(await response.text())
      return parse(await response.text())
    })
    fetched.set(path, answer)
    answer.catch(() => fetched.delete(path))
  }
  return answer
}

// fetchSheet returns a sheet's rules; one that fails to load has none.
function fetchSheet(path: string): Promise<Rule[]> {
  return fetchOnce(path, parseSheet).catch((error: unknown) => {
    console.debug(`style sheet ${path}:`, error)
    return []
  })
}

// scopes holds every loaded sheet with the element it styles, in load order,
// so a later sheet wins a tie. styled holds the panels the last restyle gave
// sheet values, so a panel the sheets no longer match loses them.
const scopes: Scope[] = []
let styled = new Set<DomPanel>()
let pending = false

// restyle runs the cascade again once the current task's changes are done,
// however many ask for it, and writes each panel's changed values. It runs
// before the browser paints, so a new panel first shows with its sheet values,
// as Panorama's do, rather than transitioning to them from the defaults.
function restyle(): void {
  if (pending) return
  pending = true
  queueMicrotask(() => {
    // Drop the sheets of panels no longer on the page.
    pending = false
    for (let i = scopes.length - 1; i >= 0; i--)
      if (!scopes[i]?.element.isConnected) scopes.splice(i, 1)

    // Write each matched panel's values, then clear those no rule matches.
    const next = new Set<DomPanel>()
    for (const [element, values] of cascade(scopes)) {
      const panel = panelOf(element)
      if (!panel) continue
      panel.applySheets(values)
      next.add(panel)
    }
    for (const panel of styled)
      if (!next.has(panel)) panel.applySheets(new Map())
    styled = next
  })
}

// A new panel, a class change or a change of hover, press or focus can change
// which rules match; with no sheet loaded there is nothing to restyle.
const watch = () => {
  if (scopes.length) restyle()
}
if (typeof document !== 'undefined') {
  new MutationObserver(watch).observe(document, {
    subtree: true,
    childList: true,
    attributes: true,
    attributeFilter: ['class', 'aria-pressed'],
  })
  for (const event of [
    'mouseover',
    'mouseout',
    'mousedown',
    'mouseup',
    'focusin',
    'focusout',
  ])
    document.addEventListener(event, watch, true)
}

// nonPanels are the layout elements that hold a panel's resources rather
// than panels.
const nonPanels = new Set(['styles', 'scripts', 'snippets'])

/**
 * domPanorama is the part of Panorama a Drawing uses, on DomPanels. Debug
 * messages go to the console; a press reaches the page through the Drawing's
 * Press, so dispatched events go nowhere.
 */
export const domPanorama = {
  CreatePanel(
    type: string,
    parent: DomPanel,
    id: string,
    properties?: Record<string, string>,
  ): DomPanel {
    // Make the panel, on a form element for an input, then show its scaling,
    // hero and account as the page can.
    const panel = new DomPanel(type, id, control(type, properties ?? {}))
    panel.SetParent(parent)
    if (properties?.scaling) {
      panel.element.style.backgroundSize = backgroundSize(properties.scaling)
    }
    if (properties?.heroid) {
      panel.element.dataset.hero = `hero ${properties.heroid}`
    }
    if (properties?.accountid) {
      panel.element.dataset.account = properties.accountid
    }
    return panel
  },
  // The page has no game text, so a token shows itself and its values.
  Localize(token: string, panel: DomPanel) {
    const values = Object.values(panel.dialogVariables())
    return values.length ? `${token} ${values.join(' ')}` : token
  },
  // A text tooltip shows as the element's title; other events go nowhere.
  DispatchEvent(event: string, ...values: unknown[]) {
    const [panel, text] = values
    if (!(panel instanceof DomPanel)) return
    if (event === 'UIShowTextTooltip') panel.element.title = String(text)
    if (event === 'UIHideTextTooltip') panel.element.removeAttribute('title')
  },
  Msg(...values: unknown[]) {
    console.debug(...values)
  },
  Schedule(seconds: number, callback: () => void) {
    setTimeout(callback, seconds * 1000)
  },
} satisfies Panorama

// control returns the form element a panel of type draws on, made with
// properties as the game makes it, or undefined for a plain panel.
function control(
  type: string,
  properties: Record<string, string>,
): HTMLElement | undefined {
  switch (type) {
    case 'TextEntry': {
      const entry = document.createElement('input')
      entry.maxLength = Number(properties.maxchars ?? 100)
      entry.placeholder = properties.placeholder ?? ''
      return entry
    }
    case 'Slider': {
      // A zero increment slides smoothly.
      const slider = document.createElement('input')
      slider.type = 'range'
      slider.min = properties.min ?? '0'
      slider.max = properties.max ?? '1'
      slider.step =
        Number(properties.increment) > 0 ? properties.increment! : 'any'
      return slider
    }
    case 'ToggleButton': {
      const toggle = document.createElement('button')
      toggle.textContent = properties.text ?? ''
      return toggle
    }
    case 'DropDown':
      return document.createElement('select')
    default:
      return undefined
  }
}

// backgroundSize maps an image's Panorama scaling to a CSS background size.
function backgroundSize(scaling: string): string {
  switch (scaling) {
    case 'stretch-to-cover-preserve-aspect':
    case 'cover':
      return 'cover'
    case 'stretch':
      return '100% 100%'
    case 'stretch-to-fit-x-preserve-aspect':
      return '100% auto'
    case 'stretch-to-fit-y-preserve-aspect':
      return 'auto 100%'
    case 'none':
      return 'auto'
    default:
      return 'contain'
  }
}

// filters holds each element's Panorama color filters by property, since CSS
// writes them all in one filter.
const filters = new WeakMap<HTMLElement, Map<string, string>>()

// filter sets one of an element's color filters and writes them all.
function filter(element: HTMLElement, name: string, value: string): void {
  // Write the filter as CSS, dropping one at its identity value.
  const each = filters.get(element) ?? new Map<string, string>()
  filters.set(element, each)
  const blur = /gaussian\(\s*([\d.]+)/.exec(value)?.[1]
  const css =
    name === 'blur'
      ? blur && `blur(${blur}px)`
      : value && value !== '1' && `${name}(${value})`
  if (css) each.set(name, css)
  else each.delete(name)
  element.style.filter = [...each.values()].join(' ')
}

// weights maps Panorama's font weights to CSS's.
const weights: Record<string, string> = {
  thin: '100',
  light: '300',
  normal: '400',
  medium: '500',
  'semi-bold': '600',
  bold: '700',
  black: '900',
}

// camel returns a style property's name as code writes it.
function camel(kebab: string): string {
  return kebab.replace(/-([a-z])/g, (_, letter: string) => letter.toUpperCase())
}

// panelOf returns the stand-in panel an element belongs to, if any.
function panelOf(element: Element | null | undefined): DomPanel | null {
  return (element as (Element & { panel?: DomPanel }) | null)?.panel ?? null
}

// applyStyle translates one Panorama style property, named in either case
// the engine accepts, into CSS.
function applyStyle(element: HTMLElement, kebab: string, value: string): void {
  const css = element.style
  const name = camel(kebab)
  switch (name) {
    case 'flowChildren': {
      element.classList.remove(
        'flow-none',
        'flow-down',
        'flow-right',
        'flow-right-wrap',
        'flow-down-wrap',
      )
      element.classList.add(`flow-${value || 'none'}`)
      return
    }
    case 'horizontalAlign':
    case 'verticalAlign': {
      const key = name === 'horizontalAlign' ? 'ha' : 'va'
      if (value) element.dataset[key] = value
      else delete element.dataset[key]
      return
    }
    case 'width':
    case 'height': {
      const grow = /^fill-parent-flow\(\s*([\d.]+)\s*\)$/.exec(value)
      element.classList.toggle(`grow-${name}`, !!grow)
      if (grow) {
        css.setProperty('--grow', grow[1] ?? '1')
        css.removeProperty(name)
      } else {
        css.setProperty(name, value === 'fit-children' ? '' : value)
      }
      return
    }
    case 'overflow': {
      const [x = '', y = x] = value.split(/\s+/)
      const map = (part: string) =>
        part === 'scroll' ? 'auto' : part === 'noclip' ? 'visible' : 'hidden'
      css.overflowX = value ? map(x) : ''
      css.overflowY = value ? map(y) : ''
      return
    }
    case 'x':
    case 'y': {
      // A panel outside its parent's flow sits at its offset.
      css.position = value ? 'absolute' : ''
      css[name === 'x' ? 'left' : 'top'] = value
      return
    }
    case 'textShadow': {
      // Panorama adds a strength term after the blur radius.
      css.textShadow = value.replace(/^(\S+ \S+ \S+) [\d.]+ (\S+)$/, '$1 $2')
      return
    }
    case 'textOverflow': {
      element.classList.toggle('ellipsis', value === 'ellipsis')
      element.classList.toggle('shrink', value === 'shrink')
      shrink(element)
      return
    }
    case 'fontSize': {
      css.fontSize = value
      element.dataset.fontSize = value
      shrink(element)
      return
    }
    case 'brightness':
    case 'saturation':
    case 'contrast':
    case 'blur': {
      filter(element, name === 'saturation' ? 'saturate' : name, value)
      return
    }
    case 'fontWeight': {
      css.fontWeight = weights[value] ?? value
      return
    }
    case 'backgroundColor': {
      css.background = value === 'none' ? '' : gradient(value)
      return
    }
    case 'border': {
      css.border = value === '0px none' ? '' : value
      return
    }
    case 'washColor':
    case 'soundOut':
      return
    default:
      css.setProperty(
        name.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`),
        value,
      )
  }
}

// shrink fits a label set to text-overflow: shrink, as Panorama does, by
// stepping its font size down from the declared one until the text fits its
// box. It waits a frame so the label is laid out, and fits it again whenever
// its box resizes, so a label measured before its row had a width, or before
// the window changed size, does not stay shrunk. A label that leaves shrink
// gets its declared size back.
const shrinking = new WeakSet<HTMLElement>()
const fitted = new WeakSet<HTMLElement>()
const resized = new ResizeObserver((entries) => {
  for (const entry of entries) shrink(entry.target as HTMLElement)
})
function shrink(element: HTMLElement): void {
  const wants = element.classList.contains('shrink')
  if ((!wants && !fitted.has(element)) || shrinking.has(element)) return
  shrinking.add(element)
  requestAnimationFrame(() => {
    // Start from the declared size; a label that left shrink stops there.
    shrinking.delete(element)
    element.style.fontSize = element.dataset.fontSize ?? ''
    if (!element.classList.contains('shrink')) {
      fitted.delete(element)
      resized.unobserve(element)
      return
    }
    if (!fitted.has(element)) {
      fitted.add(element)
      resized.observe(element)
    }

    // A label with no box yet keeps its size until the observer sees one.
    if (!element.clientWidth) return
    const fits = () =>
      element.scrollWidth <= element.clientWidth &&
      element.scrollHeight <= element.clientHeight
    let size = parseFloat(getComputedStyle(element).fontSize)
    while (!fits() && size > 6) element.style.fontSize = `${--size}px`
  })
}

// gradient converts Panorama's linear and radial gradient() syntax; plain
// colors pass through.
function gradient(value: string): string {
  // Parse a Panorama linear gradient: two endpoints, then the stops.
  const linear =
    /^gradient\(\s*linear,\s*(\S+)\s+(\S+),\s*(\S+)\s+(\S+),\s*(.*)\)$/.exec(
      value,
    )
  if (linear) {
    // Map its endpoints to a CSS angle.
    const [, x1 = '', y1 = '', x2 = '', y2 = '', stops = ''] = linear
    const angle =
      x1 === x2 ? (y1 < y2 ? '180deg' : '0deg') : x1 < x2 ? '90deg' : '270deg'
    return `linear-gradient(${angle}, ${colorStops(stops)})`
  }

  // Parse a Panorama radial gradient: its center, an offset the preview
  // ignores, its radii, then the stops.
  const radial =
    /^gradient\(\s*radial,\s*(\S+)\s+(\S+),\s*\S+\s+\S+,\s*(\S+)\s+(\S+),\s*(.*)\)$/.exec(
      value,
    )
  if (!radial) return value
  const [, x, y, rx, ry, stops = ''] = radial
  return `radial-gradient(ellipse ${rx} ${ry} at ${x} ${y}, ${colorStops(stops)})`
}

// colorStops maps Panorama's from, color-stop and to stops to CSS color stops.
function colorStops(stops: string): string {
  return [
    ...stops.matchAll(
      /(from|to|color-stop)\(\s*(?:([\d.]+)\s*,\s*)?((?:[^()]|\([^)]*\))+)\)/g,
    ),
  ]
    .map(([, kind, at, color = '']) =>
      kind === 'from'
        ? `${color.trim()} 0%`
        : kind === 'to'
          ? `${color.trim()} 100%`
          : `${color.trim()} ${Number(at) * 100}%`,
    )
    .join(', ')
}
