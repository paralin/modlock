import {
  chmod,
  mkdir,
  readdir,
  rename,
  rm,
  stat,
  writeFile,
} from 'node:fs/promises'
import { delimiter, dirname, join } from 'node:path'
import * as vscode from 'vscode'

/** releases is where Modlock publishes its command line. */
export const releases = 'https://github.com/paralin/modlock/releases'

/** platforms maps Node's platform names to the release's. */
const platforms: Partial<Record<string, string>> = {
  win32: 'windows',
  linux: 'linux',
  darwin: 'darwin',
}

/** architectures maps Node's architecture names to the release's. */
const architectures: Partial<Record<string, string>> = {
  x64: 'amd64',
  arm64: 'arm64',
}

/** release names a version directory in the extension's storage. */
const release = /^\d+\.\d+\.\d+$/

/** commandLineKey keeps the release of the managed command line, once newer than the extension's. */
const commandLineKey = 'modlock.commandLine'

/**
 * Modlock manages the modlock command line the extension runs. The
 * modlock.path setting names a creator's own; otherwise the extension
 * runs the command line of the Modlock release it belongs to, or of a newer
 * release update moved it to, since a newer command line still speaks the
 * extension's events. It keeps that copy in its storage under the release's
 * version, downloads it once, removes the copies of other releases, and puts
 * it first on PATH in VS Code's terminals, so modlock there is the same
 * command line.
 */
export class Modlock {
  /** version is this extension's release, such as 0.1.10, or 0.0.0 in development. */
  readonly version: string

  /** found is the search in progress or done; a failed one is forgotten. */
  private found: Promise<string> | undefined

  constructor(private readonly context: vscode.ExtensionContext) {
    this.version = (
      context.extension.packageJSON as { version: string }
    ).version
    context.subscriptions.push(
      vscode.workspace.onDidChangeConfiguration((change) => {
        if (change.affectsConfiguration('modlock.path')) {
          this.found = undefined
          void this.path().catch(() => undefined)
        }
      }),
    )
  }

  /** commandLine is the managed command line's release, the extension's or newer. */
  commandLine(): string {
    const updated = this.context.globalState.get<string>(commandLineKey) ?? ''
    return newer(updated, this.version) ? updated : this.version
  }

  /**
   * update moves the managed command line to release latest, such as v0.1.67,
   * when it is newer, and reports whether it did. The next run uses it.
   */
  async update(latest: string): Promise<boolean> {
    // Take only a newer release.
    const version = latest.replace(/^v/, '')
    if (!release.test(version) || !newer(version, this.commandLine())) {
      return false
    }

    // Download it, then switch to it.
    const storage = this.context.globalStorageUri.fsPath
    await download(version, join(storage, version, executable()))
    await this.context.globalState.update(commandLineKey, version)
    this.found = undefined
    await this.path()
    return true
  }

  /** configured returns the command line the setting names, or empty. */
  configured(): string {
    return (
      vscode.workspace.getConfiguration('modlock').get<string>('path') ?? ''
    )
  }

  /** path returns the command line's path, downloading it when needed. */
  path(): Promise<string> {
    this.found ??= this.find().catch((error: unknown) => {
      this.found = undefined
      throw error
    })
    return this.found
  }

  /** find returns the configured command line, or sets up the managed one. */
  private async find(): Promise<string> {
    // Run the creator's own command line, and leave the terminals' PATH alone.
    const terminals = this.context.environmentVariableCollection
    const configured = this.configured()
    if (configured) {
      terminals.clear()
      return configured
    }

    // Download the release's command line the first time, and drop the
    // copies of other releases.
    const storage = this.context.globalStorageUri.fsPath
    const version = this.commandLine()
    const path = join(storage, version, executable())
    if (!(await exists(path))) {
      await download(version, path)
    }
    await prune(storage, version)

    // Offer it as modlock in VS Code's terminals.
    terminals.description = `Runs the modlock command line the Modlock extension ${this.version} manages`
    terminals.prepend('PATH', dirname(path) + delimiter)
    return path
  }
}

/** executable is the command line's file name on this computer. */
function executable(): string {
  return process.platform === 'win32' ? 'modlock.exe' : 'modlock'
}

/** newer reports whether release a, such as 0.1.67, is after release b. */
function newer(a: string, b: string): boolean {
  // Read both as three numbers; a that is not a release is never newer.
  const parse = (v: string) => v.replace(/^v/, '').split('.').map(Number)
  const [x, y] = [parse(a), parse(b)]
  if (x.length !== 3 || x.some(Number.isNaN)) return false

  // The first part that differs decides.
  for (let i = 0; i < 3; i++) {
    const [p, q] = [x[i] ?? 0, y[i] ?? 0]
    if (p !== q) return p > q
  }
  return false
}

/** exists reports whether a file is at path. */
async function exists(path: string): Promise<boolean> {
  try {
    await stat(path)
    return true
  } catch {
    return false
  }
}

/** prune removes the command lines of releases other than version. */
async function prune(storage: string, version: string): Promise<void> {
  const entries = await readdir(storage).catch(() => [])
  for (const entry of entries) {
    if (release.test(entry) && entry !== version) {
      await rm(join(storage, entry), { recursive: true, force: true })
    }
  }
}

/**
 * download fetches the command line of the Modlock release at version to path.
 * A development build, version 0.0.0, takes the latest release.
 */
async function download(version: string, path: string): Promise<void> {
  // Name the release's file for this computer.
  const os = platforms[process.platform]
  const arch = architectures[process.arch]
  if (!os || !arch) {
    throw new Error(
      `Modlock has no command line for ${process.platform} ${process.arch}; set modlock.path to one you built`,
    )
  }
  const file = `modlock-${os}-${arch}${os === 'windows' ? '.exe' : ''}`
  const url =
    version === '0.0.0'
      ? `${releases}/latest/download/${file}`
      : `${releases}/download/v${version}/${file}`

  // Download it beside its final path, then move it into place whole.
  await vscode.window.withProgress(
    {
      location: vscode.ProgressLocation.Notification,
      title: 'Downloading the modlock command line',
    },
    async () => {
      const staged = `${path}.download`
      await fetchFile(url, staged)
      await chmod(staged, 0o755)
      await rename(staged, path)
    },
  )
}

/** fetchFile writes the body at url to path, creating its directory. */
async function fetchFile(url: string, path: string): Promise<void> {
  const response = await fetch(url)
  if (!response.ok) {
    throw new Error(
      `Download ${url}: ${response.status} ${response.statusText}`,
    )
  }
  await mkdir(dirname(path), { recursive: true })
  await writeFile(path, new Uint8Array(await response.arrayBuffer()))
}
