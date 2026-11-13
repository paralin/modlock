import * as vscode from 'vscode'

import type { Session } from './session.js'

/** Item is one row of the session tree. */
interface Item {
  /** label is the row's text. */
  label: string
  /** description follows the label in a lighter color. */
  description?: string
  /** tooltip explains the row. */
  tooltip?: string
  /** icon names a codicon. */
  icon: string
  /** context selects the row's menu: server, mod or player. */
  context?: string
  /** command runs when the row is clicked, given the row. */
  command?: string
  /** slot is a player row's slot, for the Player View. */
  slot?: number
  /** children are the rows under this one. */
  children?: Item[]
}

/**
 * Sidebar draws the running session as a tree: where the mods run, each mod
 * with its state, and each player. It redraws when the session's map, mods
 * or players change.
 */
export class Sidebar implements vscode.TreeDataProvider<Item> {
  private readonly changed = new vscode.EventEmitter<void>()

  /** onDidChangeTreeData fires when the session changes. */
  readonly onDidChangeTreeData = this.changed.event

  constructor(private readonly session: Session) {
    session.onDidChange(() => this.changed.fire())
  }

  /** getTreeItem draws one row. */
  getTreeItem(item: Item): vscode.TreeItem {
    // Make the row, expanded when it has children.
    const row = new vscode.TreeItem(
      item.label,
      item.children
        ? vscode.TreeItemCollapsibleState.Expanded
        : vscode.TreeItemCollapsibleState.None,
    )
    row.description = item.description
    row.tooltip = item.tooltip
    row.iconPath = new vscode.ThemeIcon(item.icon)
    row.contextValue = item.context

    // Clicking the row runs its command.
    if (item.command) {
      row.command = {
        command: item.command,
        title: item.label,
        arguments: [item],
      }
    }
    return row
  }

  /** getChildren returns the rows under item, or the top rows. */
  getChildren(item?: Item): Item[] {
    if (item) {
      return item.children ?? []
    }
    if (!this.session.running) {
      return []
    }
    const invite = this.invite()
    return [
      ...(invite ? [invite] : []),
      this.server(),
      this.mods(),
      this.players(),
    ]
  }

  /**
   * invite is the row friends join by while the session hosts for them: the
   * lobby's link, or the join code when there is no lobby. Clicking it copies
   * what it shows.
   */
  private invite(): Item | undefined {
    const hosting = this.session.hosting
    if (!hosting) {
      return undefined
    }
    const row = {
      label: 'Invite',
      icon: 'broadcast',
      context: 'invite',
      command: 'modlock.copyInvite',
    }
    switch (hosting.lobby?.case) {
      case 'lobbyUrl':
        return {
          ...row,
          description: hosting.lobby.value,
          tooltip: 'Copy the lobby link. Friends open it to join.',
        }
      case 'reconnecting':
        return {
          ...row,
          description: 'reconnecting to the lobby',
          tooltip: 'Copy the join code. Friends run modlock join with it.',
        }
      case 'unlisted':
        return {
          ...row,
          description: `modlock join ${hosting.joinCode ?? ''}`,
          tooltip: `${hosting.lobby.value}. Copy the join code; friends run modlock join with it.`,
        }
      default:
        return { ...row, description: 'opening a lobby', icon: 'loading~spin' }
    }
  }

  /**
   * server is the row for where the mods run: a Deadlock server on its map,
   * or the sandbox with its stand-in player. Clicking it shows the output.
   */
  private server(): Item {
    const { ready, sandbox, map } = this.session
    const row = {
      icon: ready ? 'vm-running' : 'loading~spin',
      context: 'server',
      command: 'modlock.showOutput',
    }
    if (sandbox) {
      return {
        ...row,
        label: 'Sandbox',
        description: ready ? 'without the game' : 'starting',
        tooltip: `Running without the game: ${sandbox}.`,
        icon: ready ? 'beaker' : row.icon,
      }
    }
    return {
      ...row,
      label: 'Deadlock server',
      description: ready ? map || 'running' : 'starting',
    }
  }

  /**
   * mods is the mods' row and a row per mod. A failed mod's row says why;
   * clicking a mod shows the output.
   */
  private mods(): Item {
    const icons = { started: 'pass', reloaded: 'refresh', failed: 'error' }
    return {
      label: 'Mods',
      icon: 'extensions',
      children: [...this.session.mods.values()].map((mod) => ({
        label: mod.name,
        description: mod.error ?? mod.state,
        tooltip: mod.error,
        icon: icons[mod.state],
        context: 'mod',
        command: 'modlock.showOutput',
      })),
    }
  }

  /** players is the players' row and a row per player. */
  private players(): Item {
    return {
      label: 'Players',
      description: String(this.session.players.size),
      icon: 'organization',
      children: [...this.session.players.values()]
        .sort((a, b) => a.slot - b.slot)
        .map((player) => ({
          label: player.name || `Player ${player.slot}`,
          description: this.session.sandbox ? 'stand-in' : undefined,
          icon: 'person',
          tooltip: `Show what ${player.name || 'the player'} sees`,
          context: 'player',
          command: 'modlock.showPlayerView',
          slot: player.slot,
        })),
    }
  }
}
