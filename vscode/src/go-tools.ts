import * as vscode from 'vscode'

import { Manifest_Language } from '../../src/wasm/manifest.pb.js'

import { findProjects } from './project.js'

/** wasi is the environment a Go mod builds for. */
const wasi = { GOOS: 'wasip1', GOARCH: 'wasm' }

/**
 * setGoTools points the Go tools of each workspace folder that holds a Go mod
 * at WebAssembly, so gopls resolves the mod's build as modlock does. The
 * setting belongs to the Go extension, so nothing changes until it is
 * installed. A folder whose Go tools already name an operating system keeps
 * its setting.
 */
export async function setGoTools(): Promise<void> {
  // Without the Go extension there are no Go tools to point.
  if (!vscode.extensions.getExtension('golang.go')) {
    return
  }

  for (const project of await findProjects()) {
    // Only a Go mod in a workspace folder needs the setting.
    const folder = vscode.workspace.getWorkspaceFolder(
      vscode.Uri.file(project.dir),
    )
    if (project.manifest.language !== Manifest_Language.GO || !folder) {
      continue
    }

    // Add the WebAssembly target to the folder's Go tool environment.
    const go = vscode.workspace.getConfiguration('go', folder.uri)
    const env = go.get<Record<string, string>>('toolsEnvVars') ?? {}
    if (!env.GOOS) {
      await go.update(
        'toolsEnvVars',
        { ...env, ...wasi },
        vscode.ConfigurationTarget.WorkspaceFolder,
      )
    }
  }
}
