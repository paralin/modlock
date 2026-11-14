// build bundles the extension and its Player View into dist/. With --package it
// also writes modlock.vsix at the Modlock release named by --version or the
// MODLOCK_VERSION environment variable, such as v0.1.0, whose command line the
// extension downloads. Without one it packages a development build, 0.0.0,
// which downloads the latest release's command line.

import { rm } from 'node:fs/promises'
import { parseArgs } from 'node:util'

// Read the flags.
const { values } = parseArgs({
  args: Bun.argv.slice(2),
  options: {
    package: { type: 'boolean', default: false },
    version: { type: 'string', default: process.env.MODLOCK_VERSION ?? '' },
  },
})

// Bundle the extension for VS Code's Node and the Player View for its webview.
await rm('dist', { recursive: true, force: true })
const builds = await Promise.all([
  Bun.build({
    entrypoints: ['src/extension.ts'],
    outdir: 'dist',
    target: 'node',
    format: 'cjs',
    external: ['vscode'],
  }),
  Bun.build({
    entrypoints: ['src/player-view/main.ts'],
    outdir: 'dist',
    naming: 'player-view.js',
    target: 'browser',
    format: 'iife',
  }),
])
for (const build of builds) {
  if (!build.success) {
    console.error(...build.logs)
    process.exit(1)
  }
}

// Package the bundles at the targeted Modlock version.
if (values.package) {
  const version = values.version.replace(/^v/, '') || '0.0.0'
  if (!/^\d+\.\d+\.\d+$/.test(version)) {
    console.error(`${values.version} is not a release, such as v0.1.0`)
    process.exit(1)
  }
  const vsce = Bun.spawnSync(
    [
      'bunx',
      'vsce',
      'package',
      version,
      '--no-update-package-json',
      '--no-git-tag-version',
      '--no-dependencies',
      '--skip-license',
      '--out',
      'modlock.vsix',
    ],
    { stdout: 'inherit', stderr: 'inherit' },
  )
  process.exit(vsce.exitCode)
}
