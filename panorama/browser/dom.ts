import type { Panel, Panorama } from '../src/draw.js'
import {
  type Compile,
  fillPlaces,
  LayoutScope,
  localize,
  scriptPath,
} from './layouts.js'
import { cascade, parseSheet, type Rule, type Scope } from './sheets.js'

/** mouse is the last pointer position in window pixels, as Panorama reports it. */
const mouse = { x: 0, y: 0 }

/**
 * DomPanel stands in for a Panorama panel with one HTML element, so a mod's
 * interface renders in a browser for layout work. It approximates Panorama
 * layout with the CSS in panels.css, which the page must link: children
 * overlap unless the panel flows them down or right, and fill-parent-flow
 * takes the remaining space. Fonts and engine-only styles are approximate.
 * A layout it loads draws its panels, their classes and inline styles,
 * applies its style sheets to the panel it loads into and that panel's
 * subtree, and runs its scripts with that panel as their context; the
 * installed game remains the reference.
 */
export class DomPanel implements Panel {
  readonly element: HTMLElement
  readonly style: Record<string, string>
  /** sheeted holds the values the style sheets gave this panel, by name. */
  private readonly sheeted = new Map<string, string>()
  private readonly handlers = new Map<
    string,
    (...args: unknown[]) => boolean | void
  >()
  private readonly attributes = new Map<string, string>()
  /** slots holds the class SwitchClass last set in each named slot. */
  private readonly slots = new Map<string, string>()
  /** layoutfile is the layout this panel last loaded, as Panorama reports it. */
  layoutfile = ''
  /** inline holds the styles code and the layout set, by camel-case name. */
  private readonly inline: Record<string, string>
  private valid = true
  private accepts = false
  /** options holds a dropdown's option panels by id. */
  private readonly options = new Map<string, DomPanel>()
  private dialog: Record<string, string> = {}
  /** panelData is the object Data returns, kept for the panel's lifetime. */
  private readonly panelData: Record<string, unknown> = {}
  /**
   * sourceText is the text last assigned, before dialog variables fill its
   * places, or null when none has been.
   */
  private sourceText: string | null = null
  /** markup is whether the text is HTML, as a label's html attribute. */
  private markup = false
  private steam = ''
  /** hero holds the hero id and name a hero image stands in for. */
  private readonly hero: { id?: number; name?: string } = {}
  /** caption is a text button's inner label, which holds its text. */
  private readonly caption?: DomPanel
  /**
   * layout is the layout this panel loaded, if any: its named snippet roots
   * and the scope its event attributes bind in.
   */
  private layout?: {
    readonly snippets: ReadonlyMap<string, Element>
    readonly scope: LayoutScope
  }

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
      if (paneltype === 'ToggleButton') {
        this.checked = !this.checked
        event.stopPropagation()
      }
      const activate = this.handlers.get('onactivate')
      if (activate) {
        event.stopPropagation()
        activate()
      }
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
    this.element.addEventListener(
      'wheel',
      (event) => {
        // Wheel events belong to the enabled control under the pointer.
        if (!this.enabled || event.deltaY === 0) return
        mouse.x = event.clientX
        mouse.y = event.clientY
        const handled = this.handlers.get(
          event.deltaY < 0 ? 'ScrollUp' : 'ScrollDown',
        )?.()
        if (handled) {
          event.preventDefault()
          event.stopPropagation()
        }
      },
      { passive: false },
    )

    // A text button draws its text in an inner label at the top left of its
    // content box, as in Panorama, where the button's own text-align and width
    // leave the label where it is.
    if (paneltype === 'TextButton') {
      this.caption = new DomPanel('Label', '')
      this.caption.SetParent(this)
    }
  }

  get actuallayoutwidth(): number {
    return this.element.offsetWidth
  }

  get actuallayoutheight(): number {
    return this.element.offsetHeight
  }

  get actualuiscale_x(): number {
    return this.element.getBoundingClientRect().width / this.element.offsetWidth
  }

  get actualuiscale_y(): number {
    return (
      this.element.getBoundingClientRect().height / this.element.offsetHeight
    )
  }

  get actualxoffset(): number {
    return this.element.offsetLeft
  }

  get actualyoffset(): number {
    return this.element.offsetTop
  }

  GetPositionWithinWindow(): { x: number; y: number } {
    const bounds = this.element.getBoundingClientRect()
    return { x: bounds.x, y: bounds.y }
  }

  get text(): string {
    if (this.caption) return this.caption.text
    if (this.element instanceof HTMLInputElement) return this.element.value
    return this.sourceText === null
      ? (this.element.textContent ?? '')
      : this.fillText(this.sourceText)
  }
  set text(value: string) {
    if (this.caption) {
      this.caption.text = value
      return
    }
    if (this.element instanceof HTMLInputElement) {
      this.element.value = value
      return
    }
    this.sourceText = value
    this.applyText()
  }

  get html(): boolean {
    return this.markup
  }
  set html(value: boolean) {
    this.markup = value
    this.applyText()
  }

  get steamid(): string {
    return this.steam
  }
  set steamid(value: string) {
    // The preview has no Steam pictures or names, so the account id stands
    // in on avatar and user-name panels, as a hero id does on a hero image.
    this.steam = String(value)
    this.element.dataset.account = this.steam
  }

  /**
   * SetHeroID sets the hero a hero image shows, which the page stands in for
   * with text, since it has no hero portraits.
   */
  SetHeroID(id: number) {
    this.hero.id = Math.trunc(Number(id) || 0)
    this.showHero()
  }

  get heroid(): number {
    return this.hero.id ?? 0
  }
  set heroid(value: number) {
    this.SetHeroID(value)
  }

  /** heroname is the hero's name, which the stand-in shows over its id. */
  get heroname(): string {
    return this.hero.name ?? ''
  }
  set heroname(value: string) {
    this.hero.name = String(value)
    this.showHero()
  }

  // showHero writes the hero stand-in's text: the hero's name, else its id.
  private showHero() {
    const { id, name } = this.hero
    const label = name || (id === undefined ? '' : `hero ${id}`)
    if (label) this.element.dataset.hero = label
    else delete this.element.dataset.hero
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

  /** SetDialogVariable keeps a string for Localize and {s:name} places. */
  SetDialogVariable(name: string, value: string) {
    this.setDialog(name, value)
  }

  /** SetDialogVariableInt keeps an integer for {d:name} places. */
  SetDialogVariableInt(name: string, value: number) {
    this.setDialog(name, String(Math.trunc(value)))
  }

  /** SetDialogVariableFloat keeps a number for {f:name} places. */
  SetDialogVariableFloat(name: string, value: number) {
    this.setDialog(name, String(value))
  }

  // setDialog keeps a dialog variable's value, then fills the labels in this
  // panel's subtree once the current task has set all of its variables, since
  // a label fills its places from its own panel and every panel above it.
  private setDialog(name: string, value: string) {
    if (this.dialog[name] === value) return
    this.dialog[name] = value
    if (refills.size === 0) queueMicrotask(() => DomPanel.refill())
    refills.add(this)
  }

  // refill fills again the text of each label with places in the subtree of
  // a panel whose dialog variables changed, once for each label however many
  // of its panels changed.
  private static refill() {
    const changed = [...refills]
    refills.clear()
    const filled = new Set<DomPanel>()
    for (const panel of changed) {
      const subtree = [panel.element, ...panel.element.querySelectorAll('.p')]
      for (const element of subtree) {
        const label = panelOf(element)
        if (!label?.placed || filled.has(label)) continue
        filled.add(label)
        label.applyText()
      }
    }
  }

  /** dialogVariables returns the values SetDialogVariable kept. */
  dialogVariables(): Readonly<Record<string, string>> {
    return this.dialog
  }

  /**
   * Data returns a persistent object for this panel, as Panorama's Data
   * does: the same object each call, so a script can hang its own fields
   * on it.
   */
  Data(): Record<string, unknown> {
    return this.panelData
  }

  // placed is whether this label's text has dialog variable places, which a
  // change to a variable fills again.
  private get placed(): boolean {
    return this.sourceText !== null && this.sourceText.includes('{')
  }

  // fillText fills text's places from this panel's dialog variables and
  // those of the panels above it, the nearest first.
  private fillText(text: string): string {
    return fillPlaces(text, (name) => dialogValue(this, name))
  }

  // applyText writes sourceText with its places filled, as HTML when the
  // label asked for it. A panel that never received text is left alone, so
  // filling a parent's variables cannot drop its children.
  private applyText() {
    // Write only to a label that holds text of its own.
    if (this.caption || this.element instanceof HTMLInputElement) return
    if (this.sourceText === null) return
    const filled = this.fillText(this.sourceText)
    if (this.markup) this.element.innerHTML = filled
    else this.element.textContent = filled
    shrink(this.element)
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

  DeleteAsync(seconds = 0) {
    const remove = () => {
      this.valid = false
      this.element.remove()
    }
    if (seconds <= 0) remove()
    else setTimeout(remove, seconds * 1000)
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

  /** Children returns the panel's children, in order, as a new array. */
  Children(): DomPanel[] {
    return [...this.element.children].flatMap((element) => {
      const child = panelOf(element)
      return child ? [child] : []
    })
  }

  RemoveAndDeleteChildren() {
    for (const child of this.Children()) child.DeleteAsync(0)
  }

  FindChild(id: string): DomPanel | null {
    return this.Children().find((child) => child.id === id) ?? null
  }

  FindChildTraverse(id: string): DomPanel | null {
    return panelOf(this.element.querySelector(`[data-id="${CSS.escape(id)}"]`))
  }

  ClearPanelEvent(event: string) {
    this.handlers.delete(event)
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

  BHasClass(name: string) {
    return this.element.classList.contains(name)
  }

  SetHasClass(name: string, on: boolean) {
    this.element.classList.toggle(name, on)
  }

  ToggleClass(name: string) {
    this.element.classList.toggle(name)
  }

  /**
   * SwitchClass puts name in a named slot, removing the class that slot
   * previously held, as Panorama does for turn-type and hero classes.
   */
  SwitchClass(slot: string, name: string) {
    // Swap the slot's previous class for name.
    const previous = this.slots.get(slot)
    if (previous === name) return
    if (previous) this.RemoveClass(previous)
    if (name) this.AddClass(name)
    this.slots.set(slot, name)
  }

  SetDraggable(on: boolean) {
    this.element.draggable = on
  }

  IsDraggable() {
    return this.element.draggable
  }

  SetPanelEvent(
    event: string,
    handler: (...args: unknown[]) => boolean | void,
  ) {
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

  SetAttributeInt(name: string, value: number) {
    this.attributes.set(name, String(Math.trunc(value)))
  }

  GetAttributeInt(name: string, fallback: number) {
    const stored = this.attributes.get(name)
    if (stored === undefined || stored === '') return fallback
    const value = Number(stored)
    return Number.isFinite(value) ? Math.trunc(value) : fallback
  }

  // The page shows no web page in a panel, so an HTML panel stays empty.
  SetURL(_url: string) {}

  SetImage(path: string) {
    this.element.style.backgroundImage = path
      ? `url(${JSON.stringify(path)})`
      : ''
  }

  /**
   * BLoadLayout loads a layout into this panel, as Panorama does: the
   * layout's top panel describes this one and its children become this
   * panel's, though a style the drawing already gave this panel stays. Its
   * scripts run once its panels are built, and its event attributes bind in
   * their scope. It takes a page path such as /layout/mod/board.xml or the
   * game path the page serves it at, loads in the background, and refuses
   * any other path, which a browser cannot read.
   */
  BLoadLayout(url: string, _overrideExisting?: boolean, _partial?: boolean) {
    // Resolve the page path to fetch, and report the layout's game path.
    const path = layoutPath(url)
    if (!path) return false
    this.layoutfile = layoutFile(path)
    void fetchOnce(path, (text) =>
      new DOMParser().parseFromString(text, 'text/xml'),
    )
      .then((layout) => {
        // Keep the layout's snippets and the scope its events bind in.
        if (!this.valid) return
        const root = layout.documentElement
        const scope = new LayoutScope()
        this.layout = { snippets: readSnippets(root), scope }

        // Style the panel, then build its panels, then run its scripts
        // over them.
        for (const child of root.children)
          if (child.tagName.toLowerCase() === 'styles')
            this.loadSheets(child, url)
        const top = [...root.children].find(
          (child) => !nonPanels.has(child.tagName.toLowerCase()),
        )
        if (top) this.loadElement(top, scope, new Set(Object.keys(this.inline)))
        this.loadScripts(root, url, scope)
      })
      .catch((error: unknown) => console.debug(`layout ${url}:`, error))
    return true
  }

  /**
   * BLoadLayoutSnippet loads the named snippet of the nearest layout loaded
   * into this panel or one above it, as Panorama does: the snippet's top
   * panel describes this one and its children become this panel's, and its
   * event attributes bind in that layout's scope. It returns whether the
   * snippet was found.
   */
  BLoadLayoutSnippet(name: string): boolean {
    for (const panel of ancestors(this)) {
      const layout = panel.layout
      const source = layout?.snippets.get(name)
      if (!layout || !source) continue
      this.loadElement(source, layout.scope)
      return true
    }
    return false
  }

  // loadElement gives this panel a layout element's attributes, except the
  // style properties in kept, binding its event attributes in scope, then
  // adds a panel for each of its child elements.
  private loadElement(
    source: Element,
    scope: LayoutScope,
    kept = new Set<string>(),
  ) {
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
        case 'html':
          this.html = value !== 'false'
          break
        case 'src':
          this.SetImage(value)
          break
        case 'heroid':
          this.SetHeroID(Number(value))
          break
        case 'heroname':
          this.heroname = value
          break
        case 'steamid':
          this.steamid = value
          break
        case 'hittest':
        case 'visible':
        case 'enabled':
          this[name] = value !== 'false'
          break
        case 'draggable':
          this.SetDraggable(value !== 'false')
          break
        default:
          // An event attribute, such as onactivate="RollDice()", runs its
          // code in the layout's script scope.
          if (/^on[a-z]+$/i.test(name)) {
            const event = name.toLowerCase()
            scope.bind(value, (handler) => this.SetPanelEvent(event, handler))
          }
          break
      }
    }
    for (const child of source.children) {
      if (nonPanels.has(child.tagName.toLowerCase())) continue
      const panel = new DomPanel(child.tagName, child.getAttribute('id') ?? '')
      panel.SetParent(this)
      panel.loadElement(child, scope)
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

  // loadScripts runs the scripts a layout's scripts elements include, fetched
  // from the page beside the layout at url, in order and in one scope, as the
  // game runs a layout's scripts in one context: the layout library's Modlock
  // is the next script's. It then readies the layout's event scope. A script
  // the page does not serve leaves the rest unrun, since a later one leans on
  // it, and a layout whose scripts do not run binds its events with $ alone.
  private loadScripts(root: Element, url: string, scope: LayoutScope) {
    // Find the page path of each script the layout includes.
    const build = new URL(url, location.href).search
    const paths = [...root.children]
      .filter((child) => child.tagName.toLowerCase() === 'scripts')
      .flatMap((scripts) => [...scripts.children])
      .map((include) => scriptPath(include.getAttribute('src') ?? '', build))
    const $ = layoutPanorama(this)
    if (paths.some((path) => path === '')) {
      console.debug(`layout ${url}: a script the page cannot load`)
      scope.ready(runScripts([], $))
      return
    }

    // Run them once all have loaded, and bind the events in their scope.
    void Promise.all(paths.map((path) => fetchOnce(path, (text) => text)))
      .then((sources) => {
        if (this.valid) scope.ready(runScripts(sources, $))
      })
      .catch((error: unknown) => {
        console.debug(`layout ${url}:`, error)
        scope.ready(runScripts([], $))
      })
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

// layoutPath returns the page path of a layout, from a page path or a game
// path such as file://{resources}/layout/mod/board.xml, or '' if the page
// does not serve it.
function layoutPath(url: string): string {
  if (url.startsWith('/')) return url
  const match =
    /^(?:s2r:\/\/panorama|file:\/\/\{resources\})\/layout\/(.+\.xml)(\?.*)?$/.exec(
      url,
    )
  return match ? `/layout/${match[1]}${match[2] ?? ''}` : ''
}

// layoutFile is the game path Panorama's layoutfile reports for a layout the
// page serves at path.
function layoutFile(path: string): string {
  return `file://{resources}${path.replace(/\?.*$/, '')}`
}

// runScripts runs a layout's scripts in one scope with $ as their Panorama,
// and returns the compile that runs an event attribute's code in that scope,
// where it sees the functions the scripts declared. A script that throws
// leaves a scope that holds only $.
function runScripts(sources: readonly string[], $: unknown): Compile {
  // A mod's layout script is code by design; the page runs what the game
  // would, and its last line hands back the scope's own eval.
  // oxlint-disable-next-line typescript/no-implied-eval
  const run = new Function(
    '$',
    '__modlockScope',
    `${sources.join('\n;\n')}\n;\n__modlockScope((code) => () => eval(code))`,
  ) as (panorama: unknown, scope: (compile: Compile) => void) => void
  let compile: Compile | undefined
  try {
    run($, (scoped) => {
      compile = scoped
    })
  } catch (error: unknown) {
    console.debug('layout script:', error)
  }
  return compile ?? runScripts([], $)
}

// ancestors yields panel, then each panel above it, up to the page's root.
function* ancestors(panel: DomPanel): Generator<DomPanel> {
  for (let each: DomPanel | null = panel; each; each = each.GetParent()) {
    yield each
  }
}

// refills holds the panels whose dialog variables changed in the current
// task, whose labels DomPanel fills again once the task is done.
const refills = new Set<DomPanel>()

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

// readSnippets returns the named snippet roots under a layout's snippets
// elements. A snippet's first panel describes the panel that loads it.
function readSnippets(root: Element): Map<string, Element> {
  const snippets = new Map<string, Element>()
  for (const child of root.children) {
    if (child.tagName.toLowerCase() !== 'snippets') continue
    for (const snippet of child.children) {
      if (snippet.tagName.toLowerCase() !== 'snippet') continue
      const name = snippet.getAttribute('name')
      const top = [...snippet.children].find(
        (el) => !nonPanels.has(el.tagName.toLowerCase()),
      )
      if (name && top) snippets.set(name, top)
    }
  }
  return snippets
}

/** Listener is a layout script's handler for an event nothing handled. */
interface Listener {
  readonly event: string
  readonly handler: (...values: unknown[]) => void
  readonly context: DomPanel
}

// listeners holds the layout scripts' handlers in registration order; one
// whose layout left the page hears nothing more, and one that throws keeps
// the rest from missing the event. command passes on the console
// commands the scripts dispatch.
const listeners = new Set<Listener>()
let command: (line: string) => void = () => {}

/**
 * onCommand passes each console command a layout's script dispatches, such
 * as a press for its mod, to handler.
 */
export function onCommand(handler: (line: string) => void): void {
  command = handler
}

/**
 * domPanorama is the part of Panorama a Drawing uses, on DomPanels. Debug
 * messages go to the console; a button's press reaches the page through the
 * Drawing's Press, and a layout's through onCommand.
 */
export const domPanorama = {
  MousePosition: () => mouse,
  RegisterEventHandler(
    event: string,
    panel: DomPanel,
    handler: () => boolean | void,
  ) {
    panel.SetPanelEvent(event, handler)
  },
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
    if (properties?.allowtextselection === 'true') {
      panel.element.style.userSelect = 'text'
    }
    if (properties?.scaling) {
      panel.element.style.backgroundSize = backgroundSize(properties.scaling)
    }
    if (properties?.heroid) {
      panel.SetHeroID(Number(properties.heroid))
    }
    if (properties?.accountid) {
      panel.element.dataset.account = properties.accountid
    }
    return panel
  },
  // The page has no game text, so a token shows itself with its places
  // filled, or with the panel's values when it has none.
  Localize(token: string, panel: DomPanel) {
    return localize(token, panel.dialogVariables(), (name) =>
      dialogValue(panel, name),
    )
  },
  // A console command goes to the page, a text tooltip shows as the element's
  // title, and other events reach the layout scripts listening for them.
  DispatchEvent(event: string, ...values: unknown[]) {
    // A console command, such as a layout's press, is the page's alone.
    if (event === 'CitadelConCommand') {
      command(typeof values[0] === 'string' ? values[0] : '')
      return
    }

    // Each live layout listening hears the event.
    for (const listener of listeners) {
      const { context } = listener
      if (!context.IsValid() || !context.element.isConnected) {
        listeners.delete(listener)
      } else if (listener.event === event) {
        try {
          listener.handler(...values)
        } catch (error: unknown) {
          console.debug(`layout script ${event}:`, error)
        }
      }
    }

    // A tooltip shows on the panel it names.
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

// dialogValue returns the value of the dialog variable name that panel or
// the nearest panel above it holds.
function dialogValue(panel: DomPanel, name: string): string | undefined {
  for (const each of ancestors(panel)) {
    const value = each.dialogVariables()[name]
    if (value !== undefined) return value
  }
  return undefined
}

/**
 * layoutPanorama is the $ a layout's scripts see: domPanorama with context as
 * the panel the layout loaded into, timers it can cancel, and handlers for
 * the events nothing else handles. Called with a selector #id, it finds that
 * panel in context's subtree.
 */
export function layoutPanorama(context: DomPanel) {
  const find = (selector: string) =>
    selector.startsWith('#')
      ? context.FindChildTraverse(selector.slice(1))
      : null
  return Object.assign(find, domPanorama, {
    GetContextPanel: () => context,
    Schedule: (seconds: number, callback: () => void) =>
      setTimeout(callback, seconds * 1000),
    CancelScheduled: (handle: ReturnType<typeof setTimeout>) =>
      clearTimeout(handle),
    RegisterForUnhandledEvent(
      event: string,
      handler: (...values: unknown[]) => void,
    ) {
      const listener = { event, handler, context }
      listeners.add(listener)
      return listener
    },
    UnregisterForUnhandledEvent(_event: string, listener: Listener) {
      listeners.delete(listener)
    },
  })
}

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

// Panorama resolves a percentage in margin-top or margin-bottom against the
// parent's height, where CSS resolves every percentage margin against the
// parent's width. Horizontal margins already agree. So the stand-in keeps
// each panel's authored vertical percentages and writes them as pixels of
// the parent's content height, again whenever the parent resizes.

/** VerticalMargins holds a panel's authored vertical percentage margins. */
type VerticalMargins = Partial<Record<'top' | 'bottom', string>>

const verticalMargins = new WeakMap<HTMLElement, VerticalMargins>()

/** marginScratch expands a margin declaration with the browser's own parser. */
const marginScratch = document.createElement('div').style

// marginBasis watches each panel with vertical percentage margins, to see
// it laid out under a parent, and that parent, to see the basis change.
const marginBasis = new ResizeObserver((entries) => {
  for (const { target } of entries) {
    const element = target as HTMLElement
    if (verticalMargins.has(element)) {
      if (element.parentElement) marginBasis.observe(element.parentElement)
      writeVerticalMargins(element)
    }
    for (const child of element.children) {
      if (verticalMargins.has(child as HTMLElement)) {
        writeVerticalMargins(child as HTMLElement)
      }
    }
  }
})

// applyMargin writes margin or one of its sides, named in camel case, and
// keeps its vertical percentages to resolve against the parent's height.
function applyMargin(element: HTMLElement, name: string, value: string): void {
  // Expand the value into the sides it sets.
  const kebab = name.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)
  marginScratch.cssText = ''
  marginScratch.setProperty(kebab, value)
  const sides = (['top', 'right', 'bottom', 'left'] as const).filter(
    (side) => name === 'margin' || kebab === `margin-${side}`,
  )
  const values = sides.map((side) =>
    marginScratch.getPropertyValue(`margin-${side}`),
  )

  // Write each side as authored. Hand a value the parser cannot split, or
  // rejects, to the browser whole.
  const split = !value || values.some((side) => side)
  if (split) {
    sides.forEach((side, i) =>
      element.style.setProperty(`margin-${side}`, values[i] ?? ''),
    )
  } else {
    element.style.setProperty(kebab, value)
  }

  // Keep the vertical percentages the value set.
  const vertical: VerticalMargins = { ...verticalMargins.get(element) }
  sides.forEach((side, i) => {
    if (side !== 'top' && side !== 'bottom') return
    const authored = split ? (values[i] ?? '') : ''
    if (authored.includes('%')) vertical[side] = authored
    else delete vertical[side]
  })
  if (!vertical.top && !vertical.bottom) {
    verticalMargins.delete(element)
    marginBasis.unobserve(element)
    return
  }
  verticalMargins.set(element, vertical)
  marginBasis.observe(element)
  writeVerticalMargins(element)
}

// writeVerticalMargins writes a panel's vertical percentage margins as
// pixels of its parent's content height. Under a parent whose height
// follows its children, a margin of the parent's height would change that
// height and resize it again without end, so the browser's resolution
// stays there.
function writeVerticalMargins(element: HTMLElement): void {
  const vertical = verticalMargins.get(element)
  if (!vertical) return
  const basis = verticalMarginBasis(element)
  for (const side of ['top', 'bottom'] as const) {
    const authored = vertical[side]
    if (!authored) continue
    element.style.setProperty(
      `margin-${side}`,
      basis === null
        ? authored
        : authored.replace(
            /(-?[\d.]+)%/g,
            (_, percent: string) => `${(Number(percent) / 100) * basis}px`,
          ),
    )
  }
}

// verticalMarginBasis returns the content height of a panel's parent, or
// null when the panel has no parent or the parent's height is not definite:
// neither the stage nor set by a height or fill-parent-flow.
function verticalMarginBasis(element: HTMLElement): number | null {
  // Find the parent, and whether its height is definite.
  const parent = element.parentElement
  if (!parent) return null
  const definite =
    parent.id === 'stage' ||
    parent.style.height !== '' ||
    parent.classList.contains('grow-height')
  if (!definite) return null

  // Measure the height inside its padding, where its children lay out.
  const padding = getComputedStyle(parent)
  return (
    parent.clientHeight -
    parseFloat(padding.paddingTop) -
    parseFloat(padding.paddingBottom)
  )
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
        'flow-up',
        'flow-right',
        'flow-left',
        'flow-right-wrap',
        'flow-down-wrap',
        'flow-up-wrap',
        'flow-left-wrap',
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
      const side = name === 'x' ? 'left' : 'top'
      if (value) {
        css.position = 'absolute'
        css[side] = value
      } else {
        css.removeProperty(side)
        if (
          !element.classList.contains('ignore-parent-flow') &&
          !css.left &&
          !css.top
        ) {
          css.removeProperty('position')
        }
      }
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
    case 'fontFamily': {
      css.fontFamily = value
      lineRatio(element, value)
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
    case 'preTransformRotate2d': {
      // CSS's rotate turns before the transform, as the game's does.
      css.rotate = value
      return
    }
    case 'transitionProperty': {
      css.transitionProperty = value.replace(
        /pre-transform-rotate2d/g,
        'rotate',
      )
      return
    }
    case 'clip': {
      clip(element, value)
      return
    }
    case 'visibility': {
      // Panorama's collapse hides the panel and every descendant, and
      // removes it from flow. CSS visibility is inherited and a child may
      // override it with visible, so the stand-in uses display:none.
      if (value === 'collapse') css.display = 'none'
      else css.removeProperty('display')
      css.removeProperty('visibility')
      return
    }
    case 'ignoreParentFlow': {
      // A panel that ignores its parent's flow is taken out of
      // flow-children and sits at its x/y offsets and margins.
      element.classList.toggle('ignore-parent-flow', value === 'true')
      return
    }
    case 'washColor':
    case 'soundOut':
      return
    case 'margin':
    case 'marginTop':
    case 'marginRight':
    case 'marginBottom':
    case 'marginLeft': {
      applyMargin(element, name, value)
      return
    }
    default:
      css.setProperty(
        name.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`),
        value,
      )
  }
}

// clip draws an element inside the game's clip shape: rect( top, right,
// bottom, left ), each a percent from the top or left edge, as a CSS inset,
// or radial( x y, start, sweep ) as a conic mask sweeping clockwise from
// start. Any other value clears both.
function clip(element: HTMLElement, value: string): void {
  // Read the shape's numbers in order, whatever their units.
  const css = element.style
  css.clipPath = ''
  css.maskImage = ''
  const shape = /^\s*(rect|radial)\(/.exec(value)?.[1]
  const numbers = (value.match(/-?[\d.]+/g) ?? []).map(Number)
  if (shape === 'rect' && numbers.length === 4) {
    const [top = 0, right = 100, bottom = 100, left = 0] = numbers
    css.clipPath = `inset(${top}% ${100 - right}% ${100 - bottom}% ${left}%)`
    return
  }
  if (shape !== 'radial' || numbers.length !== 4) return

  // Show the slice from start through sweep, clockwise from the top.
  const [x = 50, y = 50, start = 0, sweep = 0] = numbers
  css.maskImage = `conic-gradient(from ${start}deg at ${x}% ${y}%, #000 ${sweep}deg, transparent 0)`
}

// lineRatio sets the line height ratio of an element's font family. Panorama
// sets a line to the face's ascent plus descent plus half its line gap, which
// matches the game for every Deadlock face, and panels.css floors the ratio
// times the font size to whole pixels, as the game does. Canvas gives the
// ascent and descent, and a line at normal height adds the whole gap. The
// face's metrics are read once it loads, and an element with no family of its
// own inherits the document's, set when the page's fonts are ready.
const ratios = new Map<string, Promise<number>>()
function lineRatio(element: HTMLElement, family: string): void {
  // An element without a family of its own inherits its parent's ratio.
  element.dataset.family = family
  if (!family) {
    element.style.removeProperty('--line-ratio')
    return
  }

  // Measure each family once, and give the element its ratio unless its
  // family changed while the face loaded.
  let ratio = ratios.get(family)
  if (!ratio) {
    ratio = document.fonts.load(`100px ${family}`).then(() => faceRatio(family))
    ratios.set(family, ratio)
  }
  void ratio.then((value) => {
    if (element.dataset.family !== family) return
    element.style.setProperty('--line-ratio', String(value))
    shrink(element)
  })
}

// faceRatio returns a loaded family's ascent plus descent plus half its line
// gap, per unit of font size.
function faceRatio(family: string): number {
  // Canvas gives the face's ascent and descent.
  const context = document.createElement('canvas').getContext('2d')
  if (!context) return 1.2
  context.font = `1000px ${family}`
  const metrics = context.measureText('Hg')
  const face = metrics.fontBoundingBoxAscent + metrics.fontBoundingBoxDescent

  // A line at normal height adds the whole gap.
  const line = document.createElement('div')
  line.style.cssText = `position: absolute; visibility: hidden; font: 1000px/normal ${family}`
  line.textContent = 'Hg'
  document.body.append(line)
  const normal = line.offsetHeight
  line.remove()
  return (face + Math.max(0, normal - face) / 2) / 1000
}
void document.fonts.ready.then(() =>
  lineRatio(
    document.documentElement,
    getComputedStyle(document.body ?? document.documentElement).fontFamily,
  ),
)

// shrink fits a label set to text-overflow: shrink, as Panorama does, by
// stepping its font size down from the declared one until the text fits its
// box, while its line box keeps the declared size's height. A label whose
// height fits its text grows with it, so only a fixed height is a bound;
// measuring a fitted one would compare a fractional line box rounded two ways
// and shrink a label that fits. It waits a frame so the label is laid out,
// and fits it again whenever its box resizes, so a label measured before its
// row had a width, or before the window changed size, does not stay shrunk. A
// label that leaves shrink gets its declared size back.
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
    element.style.lineHeight = ''
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
    const fixed =
      element.style.height !== '' || element.classList.contains('grow-height')
    const fits = () =>
      element.scrollWidth <= element.clientWidth &&
      (!fixed || element.scrollHeight <= element.clientHeight)
    if (fits()) return
    const declared = getComputedStyle(element)
    element.style.lineHeight = declared.lineHeight
    let size = parseFloat(declared.fontSize)
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
