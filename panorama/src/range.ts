// A range is a track with several handles the player drags, such as a
// minimum, a favored value and a maximum. The game's slider has one thumb, so
// the drawing builds a range from stock panels: the input's panel is the
// track, holding a rail and one knob per handle, each placed at its value's
// share of the track.
//
// RangeControl.tla gives the states. While the player holds a handle the
// control moves it, and a redraw leaves the handles alone; otherwise the
// mod's tree places them. Dragging a knob starts Panorama's drag, and the
// control reads the pointer each frame until the drag ends, so a handle
// follows the pointer past the track's ends. A handle stops at its
// neighbors. Where knobs overlap, the drag takes the lowest of them when it
// starts down the track and the highest when it starts up, so the player can
// always pull the stack apart. Letting go reports every value.
//
// The track is one stop in the game's keyboard and controller navigation, so
// up and down still move between controls. While it has the focus, left and
// right step the active handle by the track's step, or a twentieth of the
// track without one, stopping at its neighbors, and each step reports every
// value at once. Enter or the controller's confirm makes the next handle
// active, and the active knob is outlined while the track has the focus. A stock
// overlay or the menu's closing cancels the hold and puts the handles back,
// and a control that goes mid-drag reports nothing, so a control drawn again
// shows the mod's values.

import {
  ActionPhase,
  type Range,
  type RangeEvent,
} from '../../src/wasm/ui/ui.pb.js'
import type { Panel, Panorama } from './draw.js'
import {
  clamp,
  finite,
  trackFraction,
  trackRange,
  trackValue,
} from './number.js'

/** RangeHost is what a range needs from the drawing around it. */
export interface RangeHost {
  /** send reports a hold to the mod. */
  send(event: RangeEvent): void
  /** live reports whether the drawing still shows this control. */
  live(): boolean
  /**
   * open reports whether the player's input is the control's: no stock
   * overlay holds it and the control's menu, if any, is open.
   */
  open(): boolean
}

/** Hold is the player's hold on the control: none, or one handle dragged. */
type Hold =
  | { readonly state: 'idle' }
  | {
      readonly state: 'dragging'
      readonly handle: number
      readonly ghost: Panel
    }

/** rangeLook is a range's size when its mod sets none. */
export const rangeLook: Record<string, string> = {
  width: '320px',
  height: '32px',
  overflow: 'noclip',
}

/** knobWidth is a knob's width in layout units; a knob centers on its value. */
const knobWidth = 16

/** knobOutline marks the knob the keyboard and controller step. */
const knobOutline = '2px solid #ffffff'

/** RangeControl draws one range and drives the player's hold on it. */
export class RangeControl {
  private readonly knobs: Panel[] = []
  private track: Range
  /** drawn holds the values the mod last drew; shown the ones on screen. */
  private drawn: number[]
  private shown: number[]
  private hold: Hold = { state: 'idle' }
  private disabled: boolean
  /** top is the knob drawn above the others, once one has moved. */
  private top: Panel | null = null
  /** active is the handle the keyboard and controller step. */
  private active = 0
  /** focused reports whether the track has the keyboard focus. */
  private focused = false

  constructor(
    private readonly panorama: Panorama,
    private readonly panel: Panel,
    range: Range,
    disabled: boolean,
    private readonly host: RangeHost,
  ) {
    // Show the mod's values as drawn.
    this.track = range
    this.disabled = disabled
    this.drawn = values(range)
    this.shown = [...this.drawn]

    // Build the rail, then a knob for each handle above it.
    this.rail()
    for (let index = 0; index < this.drawn.length; index++) {
      this.knobs.push(this.knob(index))
    }
    this.place()

    // Take the focus, step on left and right, and pass the step on with
    // confirm.
    panel.SetAcceptsFocus?.(true)
    panel.SetPanelEvent('onmoveleft', () => this.step(-1))
    panel.SetPanelEvent('onmoveright', () => this.step(1))
    panel.SetPanelEvent('onactivate', () => this.next())
    panel.SetPanelEvent('onfocus', () => this.outline(true))
    panel.SetPanelEvent('onblur', () => this.outline(false))
  }

  /**
   * step moves the active handle one step toward delta's sign, stopping at
   * its neighbors and the track's ends, and reports every value. It returns
   * whether the handle moved.
   */
  step(delta: number): boolean {
    // Step only while no hold runs and the player's input is the control's.
    if (
      this.hold.state !== 'idle' ||
      this.disabled ||
      !this.host.live() ||
      !this.host.open()
    ) {
      return false
    }

    // Find the room the active handle has between its neighbors.
    const handle = this.active
    const [min, max] = trackRange(this.track)
    const value = this.shown[handle]!
    const low = Math.max(min, this.shown[handle - 1] ?? -Infinity)
    const high = Math.min(max, this.shown[handle + 1] ?? Infinity)

    // Move by the track's step, snapped to it, within that room.
    const step = finite(this.track.step)
    const moved =
      value + Math.sign(delta) * (step > 0 ? step : (max - min) / 20)
    const next = clamp(
      trackValue(this.track, trackFraction(this.track, moved)),
      low,
      high,
    )
    if (next === value) return false

    // Show the step and report it as a hold let go at once.
    this.shown[handle] = next
    this.raise(handle)
    this.place()
    this.host.send({
      phase: ActionPhase.RELEASED,
      handle: this.name(handle),
      values: [...this.shown],
    })
    return true
  }

  /** next makes the next handle active, after the last the first. */
  next(): void {
    this.active = (this.active + 1) % this.knobs.length
    this.outline(this.focused)
  }

  /** outline marks the active knob while the track has the focus. */
  private outline(focused: boolean): void {
    this.focused = focused
    for (const [handle, knob] of this.knobs.entries()) {
      knob.style.border = focused && handle === this.active ? knobOutline : null
    }
  }

  /**
   * draw takes the mod's new range. Outside a hold, new values or bounds
   * show at once; during one, the dragged handles stay where the player has
   * them, and the mod's values show if the hold is cancelled.
   */
  draw(range: Range, disabled: boolean): void {
    // Keep the mod's range, noting whether anything it shows changed.
    const next = values(range)
    const moved =
      next.some((value, index) => value !== this.drawn[index]) ||
      finite(range.min) !== finite(this.track.min) ||
      finite(range.max) !== finite(this.track.max) ||
      finite(range.step) !== finite(this.track.step)
    this.track = range
    this.drawn = next
    this.disabled = disabled

    // A disabled control lets go; an idle one shows what changed.
    if (disabled) this.cancel()
    if (moved && this.hold.state === 'idle') {
      this.shown = [...next]
      this.place()
    }
  }

  /**
   * cancel ends a hold before the player let go, puts the handles where the
   * mod drew them and tells the mod.
   */
  cancel(): void {
    // Put the handles where the mod drew them, then tell the mod.
    if (this.hold.state !== 'dragging') return
    const { handle } = this.hold
    this.end()
    this.shown = [...this.drawn]
    this.place()
    this.host.send({
      phase: ActionPhase.CANCELLED,
      handle: this.name(handle),
      values: [],
    })
  }

  /** start takes the knob the drag picks and follows the pointer. */
  private start(index: number, drag: DragCallbacks): boolean {
    // Refuse the drag unless the player's input is free for this control.
    if (
      this.hold.state !== 'idle' ||
      this.disabled ||
      !this.host.live() ||
      !this.host.open()
    ) {
      return false
    }

    // The drag carries a panel the player cannot see, since the knob itself
    // stays on the track.
    const ghost = this.panorama.CreatePanel('Panel', this.panel, 'RangeGhost')
    Object.assign(ghost.style, { width: '1px', height: '1px', opacity: '0' })
    drag.displayPanel = ghost
    drag.offsetX = 0
    drag.offsetY = 0

    // Of knobs stacked on one value, take the one that can move the way the
    // pointer went.
    const handle = this.pick(index)
    this.hold = { state: 'dragging', handle, ghost }
    this.raise(handle)
    this.host.send({
      phase: ActionPhase.PRESSED,
      handle: this.name(handle),
      values: [],
    })
    this.follow()
    return true
  }

  /**
   * follow moves the held handle to the pointer each frame, between its
   * neighbors, and cancels the hold when the player's input leaves it.
   */
  private follow(): void {
    // A control that went mid-drag says nothing; a drawn one starts afresh.
    const hold = this.hold
    if (hold.state !== 'dragging') return
    if (!this.host.live()) {
      this.end()
      return
    }
    if (!this.host.open()) {
      this.cancel()
      return
    }

    // Place the handle under the pointer, stopping at its neighbors.
    const fraction = this.pointer()
    if (fraction !== null) {
      const low = this.shown[hold.handle - 1] ?? -Infinity
      const high = this.shown[hold.handle + 1] ?? Infinity
      this.shown[hold.handle] = clamp(
        trackValue(this.track, fraction),
        low,
        high,
      )
      this.place()
    }
    this.panorama.Schedule?.(0, () => {
      if (this.hold === hold) this.follow()
    })
  }

  /** release ends the hold where the player let go and reports every value. */
  private release(): boolean {
    // End the hold, and report it while the drawing still shows the control.
    const hold = this.hold
    if (hold.state !== 'dragging') return false
    this.end()
    if (!this.host.live()) return true
    this.host.send({
      phase: ActionPhase.RELEASED,
      handle: this.name(hold.handle),
      values: [...this.shown],
    })
    return true
  }

  /** end forgets the hold and the panel its drag carried. */
  private end(): void {
    if (this.hold.state === 'dragging' && this.hold.ghost.IsValid()) {
      this.hold.ghost.DeleteAsync(0)
    }
    this.hold = { state: 'idle' }
  }

  /**
   * pick returns which knob of the stack at index's value a drag takes: the
   * lowest when the pointer is below that value, else the highest.
   */
  private pick(index: number): number {
    // Find the stack of knobs at index's value.
    const value = this.shown[index]!
    let low = index
    let high = index
    while (low > 0 && this.shown[low - 1] === value) low--
    while (high + 1 < this.shown.length && this.shown[high + 1] === value)
      high++

    // Take its end on the side the pointer is.
    if (low === high) return index
    const fraction = this.pointer()
    if (fraction === null) return index
    return fraction < trackFraction(this.track, value) ? low : high
  }

  /** raise draws handle's knob above the others, as the last one moved. */
  private raise(handle: number): void {
    const knob = this.knobs[handle]!
    const top = this.top ?? this.knobs[this.knobs.length - 1]!
    if (knob !== top) this.panel.MoveChildAfter(knob, top)
    this.top = knob
  }

  /** place puts each knob at its shown value's share of the track. */
  private place(): void {
    for (const [handle, knob] of this.knobs.entries()) {
      const share = trackFraction(this.track, this.shown[handle] ?? 0)
      knob.style.x = `${(share * 100).toFixed(3)}%`
    }
  }

  /** pointer returns the pointer's share of the track, or null unknown. */
  private pointer(): number | null {
    // Read the pointer and the track's box in window pixels.
    const mouse = this.panorama.MousePosition?.()
    const origin = this.panel.GetPositionWithinWindow?.()
    const width =
      (this.panel.actuallayoutwidth ?? 0) * (this.panel.actualuiscale_x ?? 1)
    if (!mouse || !origin || !(width > 0) || !Number.isFinite(mouse.x)) {
      return null
    }
    return clamp((mouse.x - origin.x) / width, 0, 1)
  }

  /** rail builds the line the knobs ride on, which takes no pointer. */
  private rail(): void {
    const rail = this.panorama.CreatePanel('Panel', this.panel, 'RangeRail')
    Object.assign(rail.style, {
      width: '100%',
      height: '4px',
      verticalAlign: 'center',
      backgroundColor: 'rgba(198, 198, 198, 0.350)',
      borderRadius: '2px',
    })
    rail.hittest = false
  }

  /** knob builds the draggable knob of the handle at index. */
  private knob(index: number): Panel {
    // Build the knob, centered on its value.
    const knob = this.panorama.CreatePanel('Panel', this.panel, 'RangeKnob', {
      draggable: 'true',
    })
    Object.assign(knob.style, {
      width: `${knobWidth}px`,
      height: '100%',
      marginLeft: `${-knobWidth / 2}px`,
      backgroundColor: 'rgba(255, 239, 215, 0.900)',
      borderRadius: '3px',
    })

    // Dragging it starts a hold; letting go ends it.
    this.panorama.RegisterEventHandler?.(
      'DragStart',
      knob,
      (_id: string, drag: DragCallbacks) => this.start(index, drag),
    )
    this.panorama.RegisterEventHandler?.('DragEnd', knob, () => this.release())
    return knob
  }

  /** name returns handle's name as the mod drew it. */
  private name(handle: number): string {
    return this.track.handles?.[handle]?.name ?? ''
  }
}

/**
 * DragCallbacks is what Panorama's DragStart hands its handler: the panel
 * the drag carries under the pointer, and where it sits from the pointer.
 */
interface DragCallbacks {
  displayPanel: Panel | null
  offsetX: number
  offsetY: number
}

/** values returns a range's handle values in order, each finite. */
function values(range: Range): number[] {
  return (range.handles ?? []).map((handle) => finite(handle.value))
}

/** rangeHandles returns a range's handle names, which shape its panel. */
export function rangeHandles(range: Range): string[] {
  return (range.handles ?? []).map((handle) => handle.name ?? '')
}
