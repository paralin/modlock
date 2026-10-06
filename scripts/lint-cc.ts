// lint-cc reports the includes a C++ source in src/ or tests/ does not use,
// and removes them with --fix. clang-tidy's misc-include-cleaner, configured
// in .clang-tidy, checks each file in the native build and, for src/, in the
// Windows build that scripts/proton-build.sh makes, since much of src/ only
// compiles for Windows. An include counts as unused only when every build that
// compiles the file reports it. Build both trees first so generated headers
// exist.
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { availableParallelism } from 'node:os'
import { join, resolve } from 'node:path'

/** repo is the checkout root. */
const repo = resolve(import.meta.dir, '..')

/** sourcePattern matches the files lint-cc checks. */
const sourcePattern = new RegExp(`^${repo}/(src|tests)/`)

/** findingPattern matches one unused include in clang-tidy's output. */
const findingPattern =
  /^(\S+):(\d+):\d+: \w+: included header \S+ is not used directly/gm

/** Entry is one compile command in a compile_commands.json database. */
interface Entry {
  directory: string
  file: string
  output?: string
  command?: string
  arguments?: string[]
}

/** Database is a compile_commands.json directory and the files it compiles. */
interface Database {
  directory: string
  files: string[]
  extraArgs: string[]
}

/** output runs a command and returns its standard output. */
function output(command: string[]): string {
  return Bun.spawnSync(command, { stderr: 'inherit' }).stdout.toString()
}

/** llvmTool finds an LLVM tool on PATH or in Homebrew's keg-only llvm. */
function llvmTool(name: string): string {
  // Prefer the tool on PATH.
  const found = Bun.which(name)
  if (found) {
    return found
  }

  // Fall back to Homebrew's llvm, which it does not link onto PATH.
  const prefix = Bun.which('brew')
    ? output(['brew', '--prefix', 'llvm']).trim()
    : ''
  const keg = Bun.which(name, { PATH: join(prefix, 'bin') })
  if (!prefix || !keg) {
    throw new Error(`${name} not found: install LLVM`)
  }
  return keg
}

/** splitCommand splits a CMake compile command into its arguments. */
function splitCommand(command: string): string[] {
  return [...command.matchAll(/"((?:\\.|[^"\\])*)"|(\S+)/g)].map(
    (match) => match[1]?.replace(/\\(.)/g, '$1') ?? match[2]!,
  )
}

/** readEntries reads a build directory's compile database. */
function readEntries(build: string): Entry[] {
  const path = join(build, 'compile_commands.json')
  try {
    return JSON.parse(readFileSync(path, 'utf8')) as Entry[]
  } catch {
    throw new Error(`${path} is missing: configure and build ${build} first`)
  }
}

/**
 * zigFlags returns the include directories and definitions zig c++ adds for
 * target, without zig's own copy of clang's builtin headers, which belong to
 * zig's clang release rather than the clang-tidy that reads them.
 */
function zigFlags(target: string): string[] {
  // Ask zig for the compiler invocation it would run.
  const probe = join(repo, 'build-proton', 'tidy', 'probe.cc')
  writeFileSync(probe, '')
  const tokens = splitCommand(
    Bun.spawnSync([
      'zig',
      'c++',
      '-target',
      target,
      '-###',
      '-c',
      probe,
    ]).stderr.toString(),
  )

  // Keep its system include directories and definitions.
  const resourceDir = tokens[tokens.indexOf('-resource-dir') + 1] ?? ''
  const builtins = resolve(resourceDir, '../../zig/include')
  const flags: string[] = []
  tokens.forEach((token, index) => {
    const value = tokens[index + 1]
    if ((token === '-isystem' && value !== builtins) || token === '-D') {
      flags.push(token, value!)
    }
  })
  return flags
}

/**
 * windowsDatabase rewrites the Windows build's zig c++ commands as clang++
 * commands clang-tidy can read, and returns the database it writes.
 */
function windowsDatabase(build: string): Database {
  // Read the Windows build's commands and the target they compile for.
  const entries = readEntries(build).filter((entry) =>
    sourcePattern.test(entry.file),
  )
  const first = splitCommand(entries[0]?.command ?? '')
  const flags = zigFlags(
    first[first.indexOf('-target') + 1] ?? 'x86_64-windows-gnu',
  )

  // Replace each zig c++ driver with clang++ and zig's implicit flags.
  const rewritten = entries.map(({ directory, file, command }) => {
    const args = splitCommand(command ?? '')
    return {
      directory,
      file,
      arguments: ['clang++', '-nostdlibinc', ...flags, ...args.slice(2)],
    }
  })

  // Write the database beside the build.
  const directory = join(build, 'tidy')
  writeFileSync(
    join(directory, 'compile_commands.json'),
    JSON.stringify(rewritten),
  )
  return {
    directory,
    files: rewritten.map((entry) => entry.file),
    extraArgs: [],
  }
}

/** nativeDatabase returns the native build's database. */
function nativeDatabase(build: string): Database {
  const sysroot =
    process.platform === 'darwin'
      ? [`-extra-arg=-isysroot${output(['xcrun', '--show-sdk-path']).trim()}`]
      : []
  const files = readEntries(build)
    .map((entry) => entry.file)
    .filter((file) => sourcePattern.test(file))
  return { directory: build, files, extraArgs: sysroot }
}

/** tidy runs clang-tidy over one file and returns its unused include lines. */
async function tidy(
  clangTidy: string,
  database: Database,
  file: string,
): Promise<Set<number>> {
  // Run clang-tidy on the file.
  const child = Bun.spawn(
    [
      clangTidy,
      '-p',
      database.directory,
      '-quiet',
      ...database.extraArgs,
      file,
    ],
    {
      stdout: 'pipe',
      stderr: 'ignore',
    },
  )
  const text = await new Response(child.stdout).text()
  await child.exited

  // Keep the findings in the file itself.
  const lines = new Set<number>()
  for (const [, path, line] of text.matchAll(findingPattern)) {
    if (path === file) {
      lines.add(Number(line))
    }
  }
  return lines
}

/** unusedIncludes returns each file's include lines every database reports. */
async function unusedIncludes(
  databases: Database[],
): Promise<Map<string, number[]>> {
  // Queue every file of every database.
  const clangTidy = llvmTool('clang-tidy')
  const jobs = databases.flatMap((database) =>
    [...new Set(database.files)].map((file) => ({ database, file })),
  )
  const cores = availableParallelism()
  const workers = Math.max(1, Math.min(Math.floor(cores / 2), cores - 2))

  // Run clang-tidy on half the cores and collect each file's reports.
  const reports = new Map<string, Set<number>[]>()
  let next = 0
  const worker = async () => {
    while (next < jobs.length) {
      const { database, file } = jobs[next++]!
      const lines = await tidy(clangTidy, database, file)
      reports.set(file, [...(reports.get(file) ?? []), lines])
    }
  }
  await Promise.all(Array.from({ length: workers }, worker))

  // Keep the lines every build reports.
  const unused = new Map<string, number[]>()
  for (const [file, [first, ...rest]] of reports) {
    const lines = [...first!].filter((line) =>
      rest.every((other) => other.has(line)),
    )
    if (lines.length !== 0) {
      unused.set(
        file,
        lines.sort((a, b) => a - b),
      )
    }
  }
  return unused
}

/**
 * removeLines deletes the given one-based lines from file, then lets
 * clang-format merge the blank lines an emptied include group leaves.
 */
function removeLines(file: string, text: string[], lines: number[]): void {
  const removed = new Set(lines)
  writeFileSync(
    file,
    text.filter((_, index) => !removed.has(index + 1)).join('\n'),
  )
  output([llvmTool('clang-format'), '-i', file])
}

/** main reports or removes unused includes and exits 1 on an unfixed one. */
async function main(): Promise<void> {
  // Gather the native and Windows databases.
  const fix = process.argv.includes('--fix')
  const windows = join(repo, 'build-proton')
  mkdirSync(join(windows, 'tidy'), { recursive: true })
  const databases = [
    nativeDatabase(join(repo, 'build')),
    windowsDatabase(windows),
  ]

  // Report every unused include, removing it with --fix.
  const unused = await unusedIncludes(databases)
  for (const [file, lines] of [...unused].sort(([a], [b]) =>
    a.localeCompare(b),
  )) {
    const text = readFileSync(file, 'utf8').split('\n')
    for (const line of lines) {
      console.log(
        `${file.slice(repo.length + 1)}:${line}: ${text[line - 1]} is unused`,
      )
    }
    if (fix) {
      removeLines(file, text, lines)
    }
  }
  if (unused.size !== 0 && !fix) {
    console.error('Remove them with: bun run lint:cc --fix')
    process.exit(1)
  }
}

await main()
