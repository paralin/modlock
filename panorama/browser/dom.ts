import type { Panel, Panorama } from '../src/draw.js'

/**
 * DomPanel stands in for a Panorama panel with one HTML element, so a mod's
 * interface renders in a browser for layout work. It approximates Panorama
 * layout with the CSS in panels.css, which the page must link: children
 * overlap unless the panel flows them down or right, and fill-parent-flow
 * takes the remaining space. Fonts and engine-only styles are approximate,
 * and a layout it loads draws its panels, their classes and inline styles but
 * runs none of its scripts or style sheets; the installed game remains the
 * reference.
 */
export class DomPanel implements Panel {
  readonly element: HTMLElement
  readonly style: Record<string, string>
  private readonly handlers = new Map<string, (...args: unknown[]) => void>()
  private readonly attributes = new Map<string, string>()
  private valid = true
  private accepts = false

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

    // Engine style writes arrive one property at a time.
    const element = this.element
    this.style = new Proxy<Record<string, string>>(
      {},
      {
        set(target, name: string, value: string) {
          target[name] = value
          applyStyle(element, name, value)
          return true
        },
        deleteProperty(target, name: string) {
          delete target[name]
          applyStyle(element, name, '')
          return true
        },
      },
    )

    // Native activation, hover and focus events map to their DOM equivalents.
    this.element.addEventListener('click', (event) => {
      if (!this.enabled) return
      event.stopPropagation()
      this.handlers.get('onactivate')?.()
    })
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
    return this.element.textContent ?? ''
  }
  set text(value: string) {
    this.element.textContent = value
    shrink(this.element)
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
    void fetch(url)
      .then(async (response) => {
        if (!response.ok) throw new Error(await response.text())
        return new DOMParser().parseFromString(
          await response.text(),
          'text/xml',
        )
      })
      .then((layout) => {
        const top = [...layout.documentElement.children].find(
          (child) => !nonPanels.has(child.tagName.toLowerCase()),
        )
        if (this.valid && top)
          this.loadElement(top, new Set(Object.keys(this.style)))
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
    // Make the panel, then show its scaling and hero as the page can.
    const panel = new DomPanel(type, id)
    panel.SetParent(parent)
    if (properties?.scaling) {
      panel.element.style.backgroundSize = backgroundSize(properties.scaling)
    }
    if (properties?.heroid) {
      panel.element.dataset.hero = `hero ${properties.heroid}`
    }
    return panel
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
} satisfies Panorama

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
