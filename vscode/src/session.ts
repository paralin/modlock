import * as vscode from 'vscode'

import type { Hosting } from '../../cli/cli.pb.js'
import {
  type HostEvent,
  MessageKind,
} from '../../src/host_app/control/control.pb.js'
import type { Change, Node, Screen } from '../../src/wasm/ui/ui.pb.js'

/** ModState is a mod's last reported state in the server. */
export interface ModState {
  /** name is the mod's name. */
  name: string
  /** state is how the mod last changed. */
  state: 'started' | 'reloaded' | 'failed'
  /** error explains a failure. */
  error?: string
}

/** Player is a player in the server. */
export interface Player {
  /** slot is the player's slot. */
  slot: number
  /** name is the player's name. */
  name: string
}

/** Line is one line of a player's transcript. */
export interface Line {
  /** id orders the session's lines, so a page draws each once. */
  id: number
  /** mod showed the line; it is empty for the player's own command. */
  mod: string
  /** kind is where the player saw the line, or a command they typed. */
  kind: 'chat' | 'center' | 'announcement' | 'command'
  /** title heads an announcement. */
  title: string
  /** text is the line's words. */
  text: string
}

/** kinds names each message kind as a line's kind. */
const kinds = {
  [MessageKind.CHAT]: 'chat',
  [MessageKind.CENTER]: 'center',
  [MessageKind.ANNOUNCEMENT]: 'announcement',
} as const

/** keptLines bounds each player's transcript. */
const keptLines = 200

/**
 * Session follows a running server from its host events: its map, its mods,
 * its players, the interface each mod shows each player and the messages each
 * player sees. It holds the state the sidebar and the Player View draw, so
 * both agree.
 */
export class Session {
  /** running is true from a session's start until clear. */
  running = false
  /** ready is true once the server or the sandbox has started. */
  ready = false
  /**
   * sandbox explains why the mods run in the sandbox, without the game; it is
   * empty while they run in a Deadlock server.
   */
  sandbox = ''
  /** map is the loaded map, empty until ready or when no mod names one. */
  map = ''
  /** mods holds each mod's state by name. */
  readonly mods = new Map<string, ModState>()
  /** players holds each player by slot. */
  readonly players = new Map<number, Player>()
  /** hosting is how friends join, while the session hosts for them. */
  hosting: Hosting | undefined

  /** trees holds each player's nodes by id, per mod, as the host keeps them. */
  private readonly trees = new Map<number, Map<string, Map<string, Node>>>()
  /** transcripts holds each player's latest lines by slot. */
  private readonly transcripts = new Map<number, Line[]>()
  /** lines counts the session's lines, to number the next. */
  private lines = 0
  private readonly changed = new vscode.EventEmitter<void>()
  private readonly seen = new vscode.EventEmitter<number>()

  /** onDidChange fires when the map, a mod or a player changes. */
  readonly onDidChange = this.changed.event
  /**
   * onDidChangeSeen fires with a slot when what that player sees changes: an
   * interface or a new line.
   */
  readonly onDidChangeSeen = this.seen.event

  /** start marks a new session running. */
  start(): void {
    this.clear()
    this.running = true
    this.changed.fire()
  }

  /** clear forgets the session once its server stops. */
  clear(): void {
    // Forget the session's state, mods and players.
    this.running = false
    this.ready = false
    this.sandbox = ''
    this.map = ''
    this.mods.clear()
    this.players.clear()
    this.hosting = undefined

    // Drop each player's screen and transcript, and tell the views.
    const slots = new Set([...this.trees.keys(), ...this.transcripts.keys()])
    this.trees.clear()
    this.transcripts.clear()
    for (const slot of slots) {
      this.seen.fire(slot)
    }
    this.changed.fire()
  }

  /** hosted records how friends join the hosted session now. */
  hosted(hosting: Hosting): void {
    this.hosting = hosting
    this.changed.fire()
  }

  /** sandboxed marks the session as running in the sandbox, for reason. */
  sandboxed(reason: string): void {
    this.sandbox = reason
    this.changed.fire()
  }

  /** said records a command the player in slot typed. */
  said(slot: number, text: string): void {
    this.add(slot, { mod: '', kind: 'command', title: '', text })
  }

  /** handle follows one host event. */
  handle(event: HostEvent): void {
    const body = event.body
    switch (body?.case) {
      case 'ready':
        this.ready = true
        this.map = body.value.map ?? ''
        break
      case 'started':
        this.setMod(body.value.mod ?? '', {
          state: body.value.reloaded ? 'reloaded' : 'started',
        })
        break
      case 'failed':
        if (body.value.mod) {
          this.setMod(body.value.mod, {
            state: 'failed',
            error: body.value.error,
          })
        }
        break
      case 'joined':
        this.players.set(body.value.slot ?? 0, {
          slot: body.value.slot ?? 0,
          name: body.value.name ?? '',
        })
        break
      case 'left': {
        const slot = body.value.slot ?? 0
        this.players.delete(slot)
        this.trees.delete(slot)
        this.transcripts.delete(slot)
        this.seen.fire(slot)
        break
      }
      case 'ui':
        this.apply(
          body.value.slot ?? 0,
          body.value.mod ?? '',
          body.value.change,
        )
        this.seen.fire(body.value.slot ?? 0)
        return
      case 'message':
        this.add(body.value.slot ?? 0, {
          mod: body.value.mod ?? '',
          kind: kinds[body.value.kind ?? MessageKind.CHAT],
          title: body.value.title ?? '',
          text: body.value.text ?? '',
        })
        return
      default:
        return
    }
    this.changed.fire()
  }

  /** screen returns every mod's tree for the player in slot. */
  screen(slot: number): Screen {
    const mods = this.trees.get(slot) ?? new Map<string, Map<string, Node>>()
    return {
      trees: [...mods].map(([mod, nodes]) => ({
        mod,
        nodes: [...nodes.values()],
      })),
    }
  }

  /** transcript returns the latest lines the player in slot saw or typed. */
  transcript(slot: number): Line[] {
    return this.transcripts.get(slot) ?? []
  }

  /** add appends a line to the transcript of the player in slot. */
  private add(slot: number, line: Omit<Line, 'id'>): void {
    const lines = this.transcripts.get(slot) ?? []
    lines.push({ id: ++this.lines, ...line })
    this.transcripts.set(slot, lines.slice(-keptLines))
    this.seen.fire(slot)
  }

  /** setMod records a mod's new state. */
  private setMod(name: string, state: Omit<ModState, 'name'>): void {
    this.mods.set(name, { name, ...state })
  }

  /** apply applies change to mod's tree for slot. */
  private apply(slot: number, mod: string, change: Change | undefined): void {
    // Find the player's trees, and start the mod's tree over on a reset.
    let mods = this.trees.get(slot)
    if (!mods) {
      mods = new Map()
      this.trees.set(slot, mods)
    }
    let nodes = mods.get(mod)
    if (!nodes || change?.reset) {
      nodes = new Map()
    }

    // Set and remove the changed nodes.
    for (const node of change?.set ?? []) {
      nodes.set(node.id ?? '', node)
    }
    for (const id of change?.removed ?? []) {
      nodes.delete(id)
    }

    // Keep only a tree that still has nodes.
    if (nodes.size === 0) {
      mods.delete(mod)
      return
    }
    mods.set(mod, nodes)
  }
}
