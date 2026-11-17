// test-luau runs a Luau test of the library, such as luau/profile_test.luau,
// with the Luau command line tool: LUAU, or luau on PATH, from the release the
// runtime compiles. The test runs against a copy of luau/modlock in which the
// host the runtime hands the library, and the clock it reads, come from the
// test's ./host module.
//
//   LUAU=/path/to/luau bun scripts/test-luau.ts luau/profile_test.luau
import {
  cpSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs'
import { tmpdir } from 'node:os'
import { basename, join, resolve } from 'node:path'

/** repo is the checkout root. */
const repo = resolve(import.meta.dir, '..')

// Copy the library and the test, with the host taken from ./host.
const test = process.argv[2] ?? 'luau/profile_test.luau'
const dir = mkdtempSync(join(tmpdir(), 'modlock-luau-'))
try {
  cpSync(join(repo, 'luau/modlock'), join(dir, 'modlock'), { recursive: true })
  const init = join(dir, 'modlock/init.luau')
  const library = readFileSync(init, 'utf8')
    .replace(/^local host = \.\.\.$/m, 'local host = require("./host")')
    .replaceAll('os.clock()', 'host.clock()')
  writeFileSync(init, library)
  writeFileSync(join(dir, 'host.luau'), 'return {}\n')
  cpSync(resolve(repo, test), join(dir, basename(test)))

  // Run the test, which ends with "ok".
  const run = Bun.spawnSync([process.env.LUAU ?? 'luau', basename(test)], {
    cwd: dir,
    stdout: 'inherit',
    stderr: 'inherit',
  })
  process.exitCode = run.exitCode ?? 1
} finally {
  rmSync(dir, { recursive: true, force: true })
}
