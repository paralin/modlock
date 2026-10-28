// rolldown.ts bundles a script mod for a release with Rolldown, which parses,
// transforms and minifies with oxc. Modlock runs it on Node, which strips the
// types, with one JSON argument: the rolldown module to import, the mod's
// directory and entry, and the built library modlock resolves to. It writes
// the minified script to standard output, or, when the mod does not bundle,
// exits 2 after writing each error as a JSON line to standard output.

import { pathToFileURL } from 'node:url'
import { stripVTControlCharacters } from 'node:util'

/** Options are what modlock passes. */
interface Options {
  rolldown: string
  dir: string
  entry: string
  library: string
}

/** Rolldown is the part of the rolldown module this script calls. */
interface Rolldown {
  rolldown: (input: object) => Promise<{
    generate: (output: object) => Promise<{ output: [{ code: string }] }>
  }>
}

/** Problem is one log Rolldown fails a build with. */
interface Problem {
  message?: string
  id?: string
  loc?: { file?: string; line?: number; column?: number }
}

const options = JSON.parse(process.argv.at(-1) ?? '{}') as Options
const { rolldown } = (await import(
  pathToFileURL(options.rolldown).href
)) as Rolldown

try {
  // Bundle the mod and the library into one minified IIFE, as the esbuild
  // dev build does without minifying.
  const bundle = await rolldown({
    input: options.entry,
    cwd: options.dir,
    platform: 'neutral',
    resolve: {
      alias: { modlock: options.library },
      mainFields: ['module', 'main'],
    },
    transform: {
      target: 'es2023',
      jsx: { runtime: 'automatic', importSource: 'modlock' },
    },
    logLevel: 'silent',
  })
  const { output } = await bundle.generate({
    format: 'iife',
    minify: true,
    comments: false,
  })
  process.stdout.write(output[0].code)
} catch (error) {
  // Write each error with its location, the message's first line uncolored.
  const problems = (error as { errors?: Problem[] }).errors ?? [
    { message: String(error) },
  ]
  for (const problem of problems) {
    const message = stripVTControlCharacters(problem.message ?? '')
    process.stdout.write(
      JSON.stringify({
        message: (message.split('\n')[0] ?? '').replace(/^\[[A-Z_]+\] /, ''),
        file: problem.loc?.file ?? problem.id ?? '',
        line: problem.loc?.line ?? 0,
        column: problem.loc?.column ?? 0,
      }) + '\n',
    )
  }
  process.exitCode = 2
}
