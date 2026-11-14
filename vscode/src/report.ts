import * as vscode from 'vscode'

import type { Built, Event, Published } from '../../cli/cli.pb.js'
import {
  type HostEvent,
  MessageKind,
  type ModSlowFrame,
  type PlayerMessage,
} from '../../src/host_app/control/control.pb.js'
import type { StackFrame } from '../../src/wasm/profile/profile.pb.js'
import { ReviewState } from '../../publish/publish.pb.js'

/** site is the hyperline.gg marketplace, modlock publish's default provider. */
const site = 'https://hyperline.gg'

/**
 * Report shows what modlock reports: notes, tool output and mod logs in the
 * Modlock output channel, each build's problems in Problems with a warning
 * on the line a slow frame spent its time in, and the session's state in the
 * status bar.
 */
export class Report implements vscode.Disposable {
  /** output is the Modlock output channel. */
  readonly output = vscode.window.createOutputChannel('Modlock')

  private readonly problems =
    vscode.languages.createDiagnosticCollection('modlock')
  /** slow warns on the hottest line of the mod's last slow frame. */
  private readonly slow =
    vscode.languages.createDiagnosticCollection('modlock-slow')
  private readonly status = vscode.window.createStatusBarItem(
    vscode.StatusBarAlignment.Left,
    10,
  )
  /** sandboxed is true while the session's mods run without the game. */
  private sandboxed = false

  constructor() {
    this.idle()
    this.status.show()
  }

  /** dispose removes the channel, the problems and the status item. */
  dispose(): void {
    this.output.dispose()
    this.problems.dispose()
    this.slow.dispose()
    this.status.dispose()
  }

  /** idle shows that no session runs. */
  idle(): void {
    this.sandboxed = false
    this.show('$(play) Modlock', 'Run the mod', 'modlock.run')
  }

  /** text shows a line a tool printed. */
  text(line: string): void {
    this.output.appendLine(line)
  }

  /** event shows one event from modlock. */
  event(event: Event): void {
    const body = event.body
    switch (body?.case) {
      case 'note':
        this.output.appendLine(body.value)
        break
      case 'building':
        this.show('$(sync~spin) Modlock: building', 'Building the mod')
        break
      case 'built':
        this.built(body.value)
        break
      case 'sandbox':
        this.sandboxed = true
        this.output.appendLine(
          `Running without the game: ${body.value.reason}. Play as the stand-in player in the Player View.`,
        )
        break
      case 'host':
        this.host(body.value)
        break
      case 'published':
        this.published(body.value)
        break
    }
  }

  /** built replaces the problems with the build's and shows its result. */
  private built(built: Built): void {
    // Group the problems by file.
    const files = new Map<string, vscode.Diagnostic[]>()
    for (const found of built.diagnostics ?? []) {
      const line = Math.max((found.line ?? 0) - 1, 0)
      const column = Math.max((found.column ?? 0) - 1, 0)
      const diagnostic = new vscode.Diagnostic(
        new vscode.Range(line, column, line, column),
        found.message ?? '',
        vscode.DiagnosticSeverity.Error,
      )
      diagnostic.source = found.source
      diagnostic.code = found.code || undefined
      const file = found.file ?? ''
      files.set(file, [...(files.get(file) ?? []), diagnostic])
    }

    // Replace the previous build's problems and slow frame.
    this.problems.clear()
    this.slow.clear()
    for (const [file, diagnostics] of files) {
      this.problems.set(vscode.Uri.file(file), diagnostics)
    }

    // Show the result.
    if (built.ok) {
      this.show('$(check) Modlock: built', 'The mod built')
      return
    }
    this.output.appendLine(`The build failed: ${built.error}`)
    this.show('$(error) Modlock: build failed', built.error ?? '')
  }

  /** host shows an event from the server. */
  private host(event: HostEvent): void {
    const body = event.body
    switch (body?.case) {
      case 'ready':
        if (this.sandboxed) {
          this.output.appendLine('The sandbox is ready.')
          this.show(
            '$(beaker) Modlock: sandbox',
            'Show what the stand-in player sees',
            'modlock.showPlayerView',
          )
          break
        }
        this.output.appendLine(`The server is ready on ${body.value.map}.`)
        this.show(
          `$(vm-running) Modlock: ${body.value.map}`,
          'The server is ready',
        )
        break
      case 'started': {
        const verb = body.value.reloaded ? 'reloaded' : 'started'
        this.output.appendLine(`${body.value.mod} ${verb}.`)
        const icon = body.value.reloaded
          ? 'refresh'
          : this.sandboxed
            ? 'beaker'
            : 'vm-running'
        this.show(`$(${icon}) Modlock: ${body.value.mod} ${verb}`, '')
        break
      }
      case 'log':
        this.output.appendLine(`[${body.value.mod}] ${body.value.text}`)
        break
      case 'message':
        this.output.appendLine(`[${body.value.mod}] ${seen(body.value)}`)
        break
      case 'failed':
        this.output.appendLine(
          body.value.mod
            ? `${body.value.mod}: ${body.value.error}`
            : (body.value.error ?? ''),
        )
        this.show(
          `$(error) Modlock: ${body.value.mod || 'server'} failed`,
          body.value.error ?? '',
        )
        break
      case 'joined':
        this.output.appendLine(`${body.value.name} joined.`)
        break
      case 'slowFrame':
        this.slowFrame(body.value)
        break
    }
  }

  /**
   * slowFrame prints a mod's slowest frame and the stacks it spent the time
   * in, and warns on the source line of the hottest one until the next build.
   */
  private slowFrame(slow: ModSlowFrame): void {
    // Describe the frame.
    const report = slow.report ?? {}
    let text = `[${slow.mod}] A frame took ${ms(report.tookMicros)} ms`
    const host = ms(report.hostMicros)
    if (host > 0) {
      text += `, ${host} ms of it in host calls`
    }
    if (report.skipped) {
      text += `; the mod skipped ${report.skipped} frames to make up for slow ones`
    }
    this.output.appendLine(`${text}.`)

    // List each stack, innermost call first.
    const total = report.samples?.total ?? 0
    const stacks = report.samples?.stacks ?? []
    for (const stack of stacks) {
      const frames = (stack.frames ?? []).slice(0, 4).map(describe).join(' < ')
      this.output.appendLine(
        `  ${stack.samples} of ${total} samples in ${frames}`,
      )
    }

    // Find the innermost source line of the hottest stack.
    this.slow.clear()
    const hottest = stacks[0]
    const at = hottest?.frames?.find(
      (frame) => frame.line && frame.file && isAbsolute(frame.file),
    )
    if (!hottest || !at?.file) {
      return
    }

    // Warn on it.
    const line = (at.line ?? 1) - 1
    const column = Math.max((at.column ?? 0) - 1, 0)
    const diagnostic = new vscode.Diagnostic(
      new vscode.Range(line, column, line, column),
      `${at.function || 'This function'} ran in ${hottest.samples} of ${total} samples of a ${ms(report.tookMicros)} ms frame, so the mod skips frames to make up for it.`,
      vscode.DiagnosticSeverity.Warning,
    )
    diagnostic.source = slow.mod
    this.slow.set(vscode.Uri.file(at.file), [diagnostic])
  }

  /** published tells the creator where the release went. */
  private published(published: Published): void {
    // An archive or GitHub release names its location.
    const release = published.release
    if (!release) {
      void vscode.window.showInformationMessage(
        `Published to ${published.provider}: ${published.location}`,
      )
      return
    }

    // A hyperline.gg release links to its page once it is approved.
    const name = `${release.slug} ${release.version}`
    switch (release.reviewState) {
      case ReviewState.APPROVED:
        void vscode.window
          .showInformationMessage(
            `Published ${name}; players can install it now.`,
            'Open Page',
          )
          .then(
            (open) =>
              open &&
              vscode.env.openExternal(
                vscode.Uri.parse(`${site}/#/mods/${release.slug}`),
              ),
          )
        break
      case ReviewState.REJECTED:
        void vscode.window.showErrorMessage(
          `${name} was rejected: ${release.reviewNote}`,
        )
        break
      default:
        void vscode.window.showInformationMessage(
          `Published ${name}; it appears once a reviewer approves it.`,
        )
    }
  }

  /** show sets the status item. */
  private show(
    text: string,
    tooltip: string,
    command = 'modlock.showOutput',
  ): void {
    this.status.text = text
    this.status.tooltip = tooltip
    this.status.command = command
  }
}

/** ms rounds microseconds to milliseconds. */
function ms(micros: bigint | undefined): number {
  return Number(((micros ?? 0n) + 500n) / 1000n)
}

/** isAbsolute reports whether file is an absolute path on any platform. */
function isAbsolute(file: string): boolean {
  return file.startsWith('/') || /^[A-Za-z]:[\\/]/.test(file)
}

/**
 * describe names a stack frame with its file relative to the workspace, such
 * as "update (src/main.ts:12)".
 */
function describe(frame: StackFrame): string {
  const name = frame.function || '<anonymous>'
  if (!frame.file) {
    return name
  }
  const file = isAbsolute(frame.file)
    ? vscode.workspace.asRelativePath(frame.file)
    : frame.file
  return frame.line ? `${name} (${file}:${frame.line})` : `${name} (${file})`
}

/** seen describes a message as the player sees it. */
function seen(message: PlayerMessage): string {
  switch (message.kind) {
    case MessageKind.CENTER:
      return `The player sees mid-screen: ${message.text}`
    case MessageKind.ANNOUNCEMENT:
      return `The player sees an announcement: ${message.title}: ${message.text}`
    default:
      return `The player sees in chat: ${message.text}`
  }
}
