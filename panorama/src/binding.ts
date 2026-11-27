// A layout's bindings set the text and visibility of panels the layout's
// author named, and take their presses, while the layout keeps its own
// fonts, art and localization. The mod names each panel by id; the drawing
// looks it up only inside the layout's panel.
//
// LayoutBinding.tla gives the states. Once the layout loads, every binding
// applies; each later draw applies what it brings. A layout's script may
// build its panels anew when it reads new data, so after the script has
// read a draw's data, a deleted bound panel makes the drawing look up every
// panel again and apply every binding. A binding writes a property
// only while it sets one, keeping the author's value to put back when the
// mod stops setting it. A press reaches the mod only from a panel the
// current draw binds to press.

import type { Binding } from '../../src/wasm/ui/ui.pb.js'
import type { Panel, Panorama } from './draw.js'

/** Bound is one named panel of a layout as the drawing last bound it. */
interface Bound {
  /** panel is the found panel, or null while the layout has none by name. */
  panel: Panel | null
  /** binding is what the mod's current draw sets on the panel. */
  binding: Binding
  /** text and visible keep the author's values while a binding sets them. */
  text: string | null
  visible: boolean | null
}

/** LayoutBindings binds the named panels of one loaded layout. */
export class LayoutBindings {
  private readonly bound = new Map<string, Bound>()
  /** listened holds each panel whose presses the drawing hears. */
  private readonly listened = new WeakSet<Panel>()
  /** missing holds each name already logged as not in the layout. */
  private readonly missing = new Set<string>()

  constructor(
    private readonly panorama: Panorama,
    private readonly layout: Panel,
    /** label names the layout in a log line, such as "arena shop.xml". */
    private readonly label: string,
    /** press sends the press of the bound panel named panel. */
    private readonly press: (panel: string) => void,
  ) {}

  /**
   * draw takes the mod's bindings, given in any order, and applies each to
   * the panel it has found.
   */
  draw(bindings: readonly Binding[]): void {
    // Put back what a dropped binding set.
    const names = new Set(bindings.map((binding) => binding.panel ?? ''))
    for (const [name, each] of this.bound) {
      if (!names.has(name)) {
        this.apply(each, {})
        this.bound.delete(name)
      }
    }

    // Keep and apply each binding that stays or arrives.
    for (const binding of bindings) {
      const name = binding.panel ?? ''
      const each = this.bound.get(name) ?? {
        panel: null,
        binding: {},
        text: null,
        visible: null,
      }
      each.binding = binding
      this.bound.set(name, each)
      this.show(name, each)
    }
  }

  /**
   * settle runs once the layout's script has read the draw's data: it finds
   * each panel it has not found, and when a found one was deleted it finds
   * and binds them all again, since the script built its panels anew.
   */
  settle(): void {
    const rebuilt = [...this.bound.values()].some(
      (each) => each.panel !== null && !each.panel.IsValid(),
    )
    for (const [name, each] of this.bound) {
      if (rebuilt || each.panel === null) this.find(name, each)
      this.show(name, each)
    }
  }

  /** show applies the entry's binding and hears its panel's presses. */
  private show(name: string, each: Bound): void {
    this.apply(each, each.binding)
    if (each.binding.press && each.panel) this.listen(name, each.panel)
  }

  /** find looks up name in the layout, logging once when it is not there. */
  private find(name: string, each: Bound): void {
    // A newly found panel has the author's values, which none was kept from.
    each.panel = this.layout.FindChildTraverse(name)
    each.text = null
    each.visible = null
    if (each.panel === null && !this.missing.has(name)) {
      this.missing.add(name)
      this.panorama.Msg(`[Modlock] ${this.label} has no panel ${name}`)
    }
  }

  /**
   * apply writes what binding sets on the entry's panel and puts back the
   * author's value of each property it no longer sets.
   */
  private apply(each: Bound, binding: Binding): void {
    // Keep the author's text before the first write, and put it back after
    // the last.
    const panel = each.panel
    if (panel === null || !panel.IsValid()) return
    if (binding.text !== undefined) {
      each.text ??= panel.text
      if (panel.text !== binding.text) panel.text = binding.text
    } else if (each.text !== null) {
      panel.text = each.text
      each.text = null
    }

    // Keep and put back its visibility the same way.
    if (binding.visible !== undefined) {
      each.visible ??= panel.visible
      if (panel.visible !== binding.visible) panel.visible = binding.visible
    } else if (each.visible !== null) {
      panel.visible = each.visible
      each.visible = null
    }
  }

  /**
   * listen hears panel's presses once, alongside any handler its author
   * gave it, and sends each while the current draw binds it to press.
   */
  private listen(name: string, panel: Panel): void {
    if (this.listened.has(panel)) return
    this.listened.add(panel)
    this.panorama.RegisterEventHandler?.('Activated', panel, () => {
      const each = this.bound.get(name)
      if (each?.panel === panel && each.binding.press && panel.IsValid()) {
        this.press(name)
      }
    })
  }
}
