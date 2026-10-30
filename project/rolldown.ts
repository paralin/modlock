// rolldown.ts bundles a script mod for a release with Rolldown, which parses,
// transforms and minifies with oxc. Modlock runs it on Node, which strips the
// types, with one JSON argument: the rolldown module to import, the mod's
// directory and entry, the built library modlock resolves to, and the mod's
// content styles directory, whose sheets a .css import adds. It writes
// the minified script to standard output, or, when the mod does not bundle,
// exits 2 after writing each error as a JSON line to standard output.

import { isAbsolute, relative, resolve, sep } from 'node:path'
import { existsSync, realpathSync } from 'node:fs'
import { pathToFileURL } from 'node:url'
import { stripVTControlCharacters } from 'node:util'

/** Options are what modlock passes. */
interface Options {
  rolldown: string
  dir: string
  entry: string
  library: string
  styles: string
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

/** PluginContext is the part of a plugin hook's context this script calls. */
interface PluginContext {
  error: (error: { message: string; id?: string }) => never
}

/** sheetPrefix marks the id of a style sheet import. */
const sheetPrefix = '\0modlock-sheet:'

/** sheetPart matches one segment of a sheet's path, as sheet accepts it. */
const sheetPart = /^[\w.-]+$/

/**
 * sheets turns each import of a .css file in the content styles directory
 * into a call that adds the sheet to every interface the mod shows, as the
 * esbuild dev build's plugin does.
 */
const sheets = {
  name: 'modlock-sheet',
  resolveId(this: PluginContext, source: string, importer?: string) {
    // Take only style sheets, and fail at the importer.
    if (!source.endsWith('.css')) {
      return null
    }
    const fail = (message: string) => this.error({ message, id: importer })

    // Find the sheet, comparing real paths, since either may run through a
    // symbolic link.
    const file = resolve(
      importer ? resolve(importer, '..') : options.dir,
      source,
    )
    if (!existsSync(file)) {
      fail(`no style sheet at ${source}`)
    }
    const styles = existsSync(options.styles)
      ? realpathSync(options.styles)
      : ''
    const rest = relative(styles, realpathSync(file))

    // Name it by its path under the styles directory, as the sheet call does.
    if (!styles || rest.startsWith('..') || isAbsolute(rest)) {
      fail(
        "a style sheet goes in content/styles/, which the build compiles into the mod's content",
      )
    }
    const path = rest.split(sep).join('/')
    if (!path.split('/').every((part) => sheetPart.test(part))) {
      fail(`a style sheet's path is letters, digits, '_', '-' and '.': ${path}`)
    }
    return sheetPrefix + path
  },
  load(id: string) {
    // Add the sheet when the mod loads.
    if (!id.startsWith(sheetPrefix)) {
      return null
    }
    const path = JSON.stringify(id.slice(sheetPrefix.length))
    return {
      code: `import { sheet } from 'modlock'\nsheet(${path})\n`,
      moduleType: 'js',
    }
  },
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
    plugins: [sheets],
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
