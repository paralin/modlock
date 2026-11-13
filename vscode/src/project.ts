import { readFile } from 'node:fs/promises'
import { dirname, join, sep } from 'node:path'
import * as vscode from 'vscode'

import { Manifest } from '../../src/wasm/manifest.pb.js'

/** Project is a mod's source directory and the manifest in its mod.json. */
export interface Project {
  /** dir is the project's directory. */
  dir: string
  /** manifest is the project's mod.json. */
  manifest: Manifest
}

/**
 * findProjects returns every project in the workspace, skipping built mods
 * and installed packages.
 */
export async function findProjects(): Promise<Project[]> {
  const files = await vscode.workspace.findFiles(
    '**/mod.json',
    '**/{build,node_modules}/**',
  )
  return Promise.all(files.map((file) => readProject(dirname(file.fsPath))))
}

/** readProject reads the project in dir. */
export async function readProject(dir: string): Promise<Project> {
  return {
    dir,
    manifest: Manifest.fromJsonString(
      await readFile(join(dir, 'mod.json'), 'utf8'),
    ),
  }
}

/**
 * pickProject returns the project the creator is working on: the one holding
 * the active editor's file, the only one in the workspace, or the one they
 * choose. It returns undefined when the workspace has none or they cancel.
 */
export async function pickProject(): Promise<Project | undefined> {
  // Find the workspace's projects.
  const projects = await findProjects()
  if (projects.length === 0) {
    void vscode.window.showErrorMessage(
      'Open a folder that holds a mod.json, or run Modlock: New Mod.',
    )
    return undefined
  }

  // Take the innermost project around the active file, then the only one.
  const active = vscode.window.activeTextEditor?.document.uri.fsPath
  const around = projects
    .filter((project) => active?.startsWith(project.dir + sep))
    .sort((a, b) => b.dir.length - a.dir.length)
  const chosen = around[0] ?? (projects.length === 1 ? projects[0] : undefined)
  if (chosen) {
    return chosen
  }

  // Let the creator choose among several.
  const picked = await vscode.window.showQuickPick(
    projects.map((project) => ({
      label: project.manifest.name || project.manifest.slug || project.dir,
      description: vscode.workspace.asRelativePath(project.dir),
      project,
    })),
    { placeHolder: 'Choose a mod' },
  )
  return picked?.project
}
