import { homedir } from 'node:os'
import * as vscode from 'vscode'

import type { Update } from '../../cli/cli.pb.js'

import type { Modlock } from './modlock.js'

/**
 * Updates keeps the modlock command line on each newer Modlock release that
 * the command line reports. The command line looks for releases itself, at
 * most daily, and reports one in every command it runs while it is out. The
 * extension itself updates through the extension store.
 *
 * With modlock.autoUpdate on, the default, it installs the release without
 * asking: the managed command line moves to it, and a creator's own runs
 * modlock update. With it off, it offers the update once per window and
 * installs it only on the creator's word.
 */
export class Updates {
  /** handled holds the releases already offered or being installed. */
  private readonly handled = new Set<string>()

  constructor(
    private readonly modlock: Modlock,
    private readonly run: (args: string[], cwd: string) => Promise<number>,
  ) {}

  /** follow installs or offers the release update names, once per window. */
  async follow(update: Update): Promise<void> {
    // Take each release once, and again after a failure.
    const latest = update.latest ?? ''
    if (!latest || this.handled.has(latest)) {
      return
    }
    this.handled.add(latest)
    try {
      if (autoUpdate()) {
        await this.install(latest)
        return
      }
      const choice = await vscode.window.showInformationMessage(
        `modlock ${latest} is out, and this window runs modlock ${update.current}.`,
        'Update',
      )
      if (choice === 'Update') {
        await this.install(latest)
      }
    } catch (error) {
      this.handled.delete(latest)
      throw error
    }
  }

  /**
   * install brings the command line to release latest. A creator's own
   * command line updates itself; the managed one moves to the release.
   */
  private async install(latest: string): Promise<void> {
    if (!this.modlock.configured()) {
      await this.modlock.update(latest)
      return
    }
    const code = await this.run(['update'], homedir())
    if (code !== 0) {
      throw new Error(
        'modlock could not update itself; see the Modlock output.',
      )
    }
  }
}

/** autoUpdate reports whether updates install without asking. */
function autoUpdate(): boolean {
  return (
    vscode.workspace.getConfiguration('modlock').get<boolean>('autoUpdate') ??
    true
  )
}
