import { type ChildProcess, spawn } from 'node:child_process'
import { createInterface } from 'node:readline'

import { Event, Input } from '../../cli/cli.pb.js'

/** Listener receives a run's events and the lines it prints for a person. */
export interface Listener {
  /** event receives each event the command reports. */
  event(event: Event): void
  /** text receives each other line: tool output and errors. */
  text(line: string): void
}

/**
 * Run is one modlock command started with --json. Its events and other output
 * reach the listener line by line until the command exits.
 */
export class Run {
  /** done settles with the exit code once the command exits. */
  readonly done: Promise<number>

  private readonly child: ChildProcess

  constructor(binary: string, args: string[], cwd: string, listener: Listener) {
    // Start the command.
    this.child = spawn(binary, ['--json', ...args], {
      cwd,
      stdio: ['pipe', 'pipe', 'pipe'],
    })

    // Read an event from each line of standard output and pass everything else
    // through as text. A newer command line's fields are skipped, so a creator's
    // own modlock may be ahead of the extension.
    createInterface({ input: this.child.stdout! }).on('line', (line) => {
      let event: Event
      try {
        event = Event.fromJsonString(line, { ignoreUnknownFields: true })
      } catch {
        listener.text(line)
        return
      }
      listener.event(event)
    })
    createInterface({ input: this.child.stderr! }).on('line', (line) =>
      listener.text(line),
    )

    // Settle once the command exits or cannot start.
    this.done = new Promise((resolve) => {
      this.child.on('error', (error) => {
        listener.text(`modlock could not start: ${error.message}`)
        resolve(1)
      })
      this.child.on('close', (code) => resolve(code ?? 1))
    })
  }

  /**
   * send writes input to the command's input. A session in the sandbox takes
   * it as the stand-in player's command or press.
   */
  send(input: Input): void {
    this.child.stdin?.write(`${Input.toJsonString(input)}\n`)
  }

  /**
   * stop closes the command's input. A play session stops its server and
   * exits; a build or publish finishes first.
   */
  stop(): void {
    this.child.stdin?.end()
  }
}
