#!/usr/bin/env node
// Runs the modlock command line that npm installed for this system, from the
// optional dependency @modlock/<platform>-<arch>, with this process's
// arguments, input and output, and exits as it exits.
import { spawnSync } from 'node:child_process'
import { createRequire } from 'node:module'

const require = createRequire(import.meta.url)
const name = `@modlock/${process.platform}-${process.arch}`
const file = process.platform === 'win32' ? 'modlock.exe' : 'modlock'

let binary
try {
  binary = require.resolve(`${name}/bin/${file}`)
} catch {
  console.error(
    `modlock has no build for ${process.platform} ${process.arch} installed; ` +
      `install modlock again without --omit=optional or --no-optional, ` +
      `or download it from https://github.com/paralin/modlock/releases`,
  )
  process.exit(1)
}

const result = spawnSync(binary, process.argv.slice(2), { stdio: 'inherit' })
if (result.error) {
  console.error(`modlock could not start ${binary}: ${result.error.message}`)
  process.exit(1)
}
if (result.signal) {
  process.kill(process.pid, result.signal)
}
process.exit(result.status ?? 1)
