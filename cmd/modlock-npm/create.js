#!/usr/bin/env node
// Starts a mod: runs `modlock new` from the @modlock/modlock this package
// depends on, with this process's arguments, input and output, and exits as
// it exits. npm create @modlock runs this.
import { spawnSync } from 'node:child_process'
import { createRequire } from 'node:module'

const require = createRequire(import.meta.url)
const modlock = require.resolve('@modlock/modlock/bin/modlock.js')

const result = spawnSync(
  process.execPath,
  [modlock, 'new', ...process.argv.slice(2)],
  {
    stdio: 'inherit',
  },
)
if (result.error) {
  console.error(
    `npm create @modlock could not start modlock: ${result.error.message}`,
  )
  process.exit(1)
}
if (result.signal) {
  process.kill(process.pid, result.signal)
}
process.exit(result.status ?? 1)
