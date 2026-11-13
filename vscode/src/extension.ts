import { join } from 'node:path'
import * as vscode from 'vscode'

import type { Hosting } from '../../cli/cli.pb.js'

import { setGoTools } from './go-tools.js'
import { Modlock } from './modlock.js'
import { PlayerView } from './player-view.js'
import { pickProject, readProject } from './project.js'
import { Report } from './report.js'
import { type Listener, Run } from './run.js'
import { Session } from './session.js'
import { Sidebar } from './sidebar.js'
import { Updates } from './updates.js'

/** languages lists the languages a new mod may use, as modlock names them. */
const languages = [
  { label: 'TypeScript', language: 'typescript' },
  { label: 'JavaScript', language: 'javascript' },
  { label: 'Luau', language: 'luau' },
  { label: 'Python', language: 'python' },
  { label: 'Go', language: 'go' },
]

/**
 * activate connects the commands, the sidebar, the Player View and the
 * reports to the modlock command line. One dev session runs at a time, in a
 * Deadlock server or in the sandbox; closing the window stops it.
 */
export function activate(context: vscode.ExtensionContext): void {
  // Make the parts the commands report to.
  const modlock = new Modlock(context)
  const report = new Report()
  const session = new Session()
  let running: Run | undefined
  let lastCommand = ''

  // Route the Player View's typed lines, presses and edits to the running
  // session.
  const say = (line: string): void => {
    // Type line as the stand-in player, the sandbox's only player.
    if (!running) {
      return
    }
    lastCommand = line
    session.said([...session.players.keys()][0] ?? 0, line)
    report.text(`Player: ${line}`)
    running.send({ body: { case: 'command', value: line } })
  }
  const playerView = new PlayerView(context.extensionUri, session, {
    command: say,
    press: (slot, mod, node) =>
      running?.send({ body: { case: 'press', value: { mod, slot, node } } }),
    change: (slot, mod, node, value) =>
      running?.send({
        body: {
          case: 'change',
          value: { mod, event: { player: slot, node, value } },
        },
      }),
  })

  // Report the session's events, and show its players in the sidebar.
  const listener: Listener = {
    event: (event) => {
      report.event(event)
      switch (event.body?.case) {
        case 'sandbox':
          // Without the game, the Player View is where the person plays.
          session.sandboxed(event.body.value.reason ?? '')
          setContext('modlock.sandbox', true)
          playerView.show()
          break
        case 'host':
          session.handle(event.body.value)
          break
        case 'hosting':
          offerInvite(session.hosting, event.body.value)
          session.hosted(event.body.value)
          break
        case 'update':
          updates
            .follow(event.body.value)
            .catch(
              (error: unknown) =>
                void vscode.window.showErrorMessage(
                  error instanceof Error ? error.message : String(error),
                ),
            )
          break
      }
    },
    text: (line) => report.text(line),
  }
  context.subscriptions.push(
    report,
    vscode.window.registerTreeDataProvider(
      'modlock.session',
      new Sidebar(session),
    ),
  )

  // Run one-shot commands to completion, and one session at a time.
  const run = async (args: string[], cwd: string): Promise<number> => {
    const started = new Run(await modlock.path(), args, cwd, listener)
    return started.done
  }
  const updates = new Updates(modlock, run)

  // Set up the command line now, so the first run does not wait for it.
  modlock
    .path()
    .catch((error: unknown) =>
      report.text(
        `Could not set up the modlock command line: ${String(error)}`,
      ),
    )

  // runMod runs the project with args, using play locally
  // and adding --lobby to let friends join.
  const runMod = async (args: string[]): Promise<void> => {
    // Stop the running session first.
    const project = await pickProject()
    if (!project) {
      return
    }
    if (running) {
      running.stop()
      await running.done
    }

    // Start the new one and follow it until it ends.
    const started = new Run(await modlock.path(), args, project.dir, listener)
    running = started
    session.start()
    setContext('modlock.running', true)
    setContext('modlock.sandbox', false)
    report.output.show(true)
    const code = await started.done
    if (running !== started) {
      return
    }

    // Return the views to idle, and report an exit with an error.
    running = undefined
    session.clear()
    report.idle()
    setContext('modlock.running', false)
    setContext('modlock.sandbox', false)
    if (code !== 0) {
      void vscode.window.showErrorMessage(
        'The Modlock session stopped with an error; see the Modlock output.',
      )
    }
  }
  context.subscriptions.push({ dispose: () => running?.stop() })

  // command registers a command that shows its failure as an error message.
  const command = (
    name: string,
    action: (...args: never[]) => unknown,
  ): void => {
    context.subscriptions.push(
      vscode.commands.registerCommand(name, async (...args: never[]) => {
        try {
          await action(...args)
        } catch (error) {
          void vscode.window.showErrorMessage(
            error instanceof Error ? error.message : String(error),
          )
        }
      }),
    )
  }

  // Make, build, run and stop mods.
  command('modlock.newMod', () => newMod(run))
  command('modlock.build', async () => {
    const project = await pickProject()
    if (project) {
      await run(['build'], project.dir)
    }
  })
  command('modlock.run', () => runMod(['play']))
  command('modlock.host', () => runMod(['play', '--lobby']))
  command('modlock.copyInvite', async () => {
    // Copy the lobby's link, or the join code without one.
    const hosting = session.hosting
    if (!hosting) {
      return
    }
    const link = hosting.lobby?.case === 'lobbyUrl' ? hosting.lobby.value : ''
    await vscode.env.clipboard.writeText(link || (hosting.joinCode ?? ''))
    void vscode.window.showInformationMessage(
      link ? 'Copied the lobby link' : 'Copied the join code',
    )
  })
  command('modlock.sendCommand', async (given?: unknown) => {
    // Take the line a keybinding passes, or ask for one.
    const line =
      typeof given === 'string'
        ? given
        : await vscode.window.showInputBox({
            prompt: 'A command the stand-in player types in chat',
            placeHolder: '/hello',
            value: lastCommand,
            valueSelection: [0, lastCommand.length],
          })
    if (line) {
      say(line)
      playerView.show()
    }
  })
  command('modlock.stop', () => running?.stop())

  // Publish mods, and show the Player View and the output.
  command('modlock.publish', async () => {
    const project = await pickProject()
    if (project) {
      report.output.show(true)
      await run(['publish'], project.dir)
    }
  })
  command('modlock.showPlayerView', (item?: { slot?: number }) =>
    playerView.show(item?.slot),
  )
  command('modlock.showOutput', () => report.output.show())

  // Offer Run Mod while the workspace holds a mod, and New Mod otherwise.
  const hasProject = async () => {
    const found = await vscode.workspace.findFiles(
      '**/mod.json',
      '**/{build,node_modules}/**',
      1,
    )
    setContext('modlock.project', found.length > 0)
  }
  const manifests = vscode.workspace.createFileSystemWatcher('**/mod.json')
  manifests.onDidCreate(hasProject)
  manifests.onDidDelete(hasProject)
  void hasProject()
  context.subscriptions.push(
    manifests,
    vscode.workspace.onDidChangeWorkspaceFolders(hasProject),
  )

  // Point Go mods' tools at WebAssembly, now and as folders or the Go
  // extension arrive.
  const goTools = () => {
    setGoTools().catch((error: unknown) =>
      report.text(
        `Could not set the Go tools for WebAssembly: ${String(error)}`,
      ),
    )
  }
  goTools()
  context.subscriptions.push(
    vscode.workspace.onDidChangeWorkspaceFolders(goTools),
    vscode.extensions.onDidChange(goTools),
  )
}

/**
 * offerInvite offers to copy the lobby's link when it first opens, or opens
 * again under a new link.
 */
function offerInvite(shown: Hosting | undefined, hosting: Hosting): void {
  // Offer each new link once.
  if (hosting.lobby?.case !== 'lobbyUrl') {
    return
  }
  const link = hosting.lobby.value
  if (shown?.lobby?.case === 'lobbyUrl' && shown.lobby.value === link) {
    return
  }
  void vscode.window
    .showInformationMessage(
      'Hosting: friends join from the lobby link.',
      'Copy Link',
    )
    .then(async (choice) => {
      if (choice) {
        await vscode.env.clipboard.writeText(link)
      }
    })
}

/** setContext sets a context key the manifest's when clauses read. */
function setContext(key: string, value: boolean): void {
  void vscode.commands.executeCommand('setContext', key, value)
}

/** deactivate leaves cleanup to the subscriptions. */
export function deactivate(): void {}

/**
 * newMod asks for a mod's language, name and parent folder, creates the
 * project with modlock new and opens it.
 */
async function newMod(
  run: (args: string[], cwd: string) => Promise<number>,
): Promise<void> {
  // Ask for the mod.
  const language = await vscode.window.showQuickPick(languages, {
    placeHolder: 'Write the mod in',
  })
  if (!language) {
    return
  }
  const name = await vscode.window.showInputBox({
    prompt: 'The mod name players see',
    validateInput: (value) =>
      slug(value) ? undefined : 'Use at least one letter or digit',
  })
  if (!name) {
    return
  }
  const parent = await vscode.window.showOpenDialog({
    canSelectFolders: true,
    canSelectFiles: false,
    openLabel: 'Create the mod here',
    defaultUri: vscode.workspace.workspaceFolders?.[0]?.uri,
  })
  if (!parent?.[0]) {
    return
  }

  // Create the project and open it. Flags precede the directory, since the
  // command line stops reading flags at its first argument.
  const dir = join(parent[0].fsPath, slug(name))
  const code = await run(
    ['new', '--name', name, '--language', language.language, dir],
    parent[0].fsPath,
  )
  if (code !== 0) {
    throw new Error('modlock could not create the mod; see the Modlock output.')
  }
  await readProject(dir)
  await vscode.commands.executeCommand(
    'vscode.openFolder',
    vscode.Uri.file(dir),
    {
      forceNewWindow: Boolean(vscode.workspace.workspaceFolders?.length),
    },
  )
}

/** slug turns a mod name into its directory name, such as "Parry Ball" into "parry-ball". */
function slug(name: string): string {
  return name
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-|-$/g, '')
}
