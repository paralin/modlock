// pre-commit checks the files a commit stages. oxfmt formats them: a file with
// no unstaged edits is formatted and restaged, and a partly staged file must
// already be formatted in the index, since restaging it would commit its
// unstaged edits. tsstyle then checks the staged TypeScript for code
// paragraphs, and oxlint lints the staged code as it stands in the working
// tree, since its type-aware rules need the project around each file.
// scripts/install-hooks.sh installs it, and bun install runs that script.
import { checkSource } from './tsstyle.js'

/** formatPattern matches the file names oxfmt formats. */
const formatPattern = /\.([cm]?[jt]sx?|json|css|html)$/

/** codePattern matches the TypeScript and JavaScript file names to lint. */
const codePattern = /\.[cm]?[jt]sx?$/

/** typescriptPattern matches the TypeScript file names tsstyle checks. */
const typescriptPattern = /\.[cm]?tsx?$/

/** foreignPattern matches paths that hold vendored, built or prototype code. */
const foreignPattern = /(^|\/)(vendor|node_modules|dist|build|prototypes)\//

/** Result is a finished command's exit code and output streams. */
interface Result {
  code: number
  stdout: string
  stderr: string
}

/** run runs a command in the checkout, feeding it stdin when given. */
function run(command: string[], stdin?: string): Result {
  const child = Bun.spawnSync(command, {
    stdin: stdin === undefined ? 'ignore' : Buffer.from(stdin),
    stdout: 'pipe',
    stderr: 'pipe',
  })
  return {
    code: child.exitCode,
    stdout: child.stdout.toString(),
    stderr: child.stderr.toString(),
  }
}

/** listFiles returns the NUL separated paths a git command prints. */
function listFiles(command: string[]): string[] {
  return run(command).stdout.split('\0').filter(Boolean)
}

/** stagedText returns a file's content as the index holds it. */
function stagedText(path: string): string {
  return run(['git', 'show', `:${path}`]).stdout
}

/**
 * formatStaged formats the staged files and reports whether every partly
 * staged file was already formatted in the index.
 */
function formatStaged(files: string[], unstaged: Set<string>): boolean {
  // Format and restage the files whose working copy matches the index.
  const whole = files.filter((file) => !unstaged.has(file))
  if (whole.length !== 0) {
    run(['bunx', 'oxfmt', '--no-error-on-unmatched-pattern', ...whole])
    run(['git', 'add', '--', ...whole])
  }

  // Compare each partly staged file's index copy with its formatted output.
  let clean = true
  for (const path of files.filter((file) => unstaged.has(file))) {
    const staged = stagedText(path)
    const formatted = run(['bunx', 'oxfmt', `--stdin-filepath=${path}`], staged)
    if (formatted.code === 0 && formatted.stdout !== staged) {
      console.error(`${path}: format it with bunx oxfmt and restage it`)
      clean = false
    }
  }
  return clean
}

/** checkParagraphs prints the tsstyle findings of the staged files. */
function checkParagraphs(files: string[]): boolean {
  let clean = true
  for (const path of files.filter((file) => typescriptPattern.test(file))) {
    for (const { line, column, message } of checkSource(
      path,
      stagedText(path),
    )) {
      console.error(`${path}:${line}:${column}: ${message}`)
      clean = false
    }
  }
  return clean
}

/** lint runs oxlint over files and prints its findings when it fails. */
function lint(files: string[]): boolean {
  // Pass when oxlint accepts every file.
  const result = run([
    'bunx',
    'oxlint',
    '--no-error-on-unmatched-pattern',
    ...files,
  ])
  if (result.code === 0) {
    return true
  }

  // Show the findings and the command that fixes what it can.
  console.error((result.stdout + result.stderr).trim())
  console.error(`Fix with: bunx oxlint --fix ${files.join(' ')}`)
  return false
}

/** main checks the staged files and exits with status 1 on any finding. */
function main(): void {
  // Select the staged handwritten files.
  const staged = listFiles([
    'git',
    'diff',
    '--cached',
    '--name-only',
    '--diff-filter=ACMR',
    '-z',
  ]).filter((file) => !foreignPattern.test(file))
  const unstaged = new Set(listFiles(['git', 'diff', '--name-only', '-z']))
  const code = staged.filter((file) => codePattern.test(file))

  // Run every check so one commit attempt shows all findings.
  const results = [
    formatStaged(
      staged.filter((file) => formatPattern.test(file)),
      unstaged,
    ),
    checkParagraphs(code),
    code.length === 0 || lint(code),
  ]
  if (results.includes(false)) {
    process.exit(1)
  }
}

main()
