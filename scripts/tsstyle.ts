// tsstyle checks that TypeScript function bodies read as commented code
// paragraphs.
//
//   bun scripts/tsstyle.ts <file-or-directory>...
//
// A body of five or more top-level statements is a sequence of actions. Blank
// lines split it into paragraphs, and each paragraph needs a purpose comment
// directly above it, except a lone return or a run of test declarations. A
// purpose comment follows a blank line, and a paragraph of more than eight
// statements holds more than one action. Findings print as file:line:column:
// message, and the command exits with status 3 when it finds any.
import { readdirSync, readFileSync, statSync } from 'node:fs'
import { join } from 'node:path'

import { parse } from '@babel/parser'
import type { BlockStatement, Node, Statement } from '@babel/types'

/** paragraphMinStatements is the statement count at which a body is multi-action. */
const paragraphMinStatements = 5

/** paragraphMaxStatements is the largest paragraph that can be one action. */
const paragraphMaxStatements = 8

/** skippedDirs holds directory names that never contain handwritten sources. Hidden directories are skipped too. */
const skippedDirs = new Set(['node_modules', 'dist', 'vendor', 'build'])

/** sourcePattern matches TypeScript source file names. */
const sourcePattern = /\.[cm]?tsx?$/

/** generatedPattern matches generated or declaration file names. */
const generatedPattern = /(\.pb\.ts|\.d\.[cm]?ts)$/

/** directivePattern matches tool directives, which are not purpose comments. */
const directivePattern =
  /^\/[/*]\s*(@ts-|eslint-|oxlint-|prettier-|biome-|istanbul )/

/** markerPattern matches TODO and XXX markers, which are not purpose comments. */
const markerPattern = /^\/[/*]\s*(TODO|XXX):/

/** functionTypes are the node types whose block body is a function body. */
const functionTypes = new Set([
  'FunctionDeclaration',
  'FunctionExpression',
  'ArrowFunctionExpression',
  'ObjectMethod',
  'ClassMethod',
  'ClassPrivateMethod',
])

/** testCallees names the test framework calls that declare tests and hooks. */
const testCallees = new Set([
  'describe',
  'test',
  'it',
  'bench',
  'beforeAll',
  'beforeEach',
  'afterAll',
  'afterEach',
])

/** Finding is one code-paragraph violation. */
export interface Finding {
  /** line is the 1-based line of the violation. */
  line: number
  /** column is the 1-based column of the violation. */
  column: number
  /** message describes the violation. */
  message: string
}

/** Paragraph is a run of statements between blank lines. */
interface Paragraph {
  /** statements are the paragraph's top-level statements in order. */
  statements: Statement[]
  /** purpose reports whether a purpose comment sits directly above it. */
  purpose: boolean
}

/**
 * checkSource returns the code-paragraph findings in one source text. A
 * generated file returns no findings, and a file that does not parse returns
 * the parse error.
 */
export function checkSource(path: string, text: string): Finding[] {
  // Generated code follows its generator's layout.
  if (
    generatedPattern.test(path) ||
    /^\/\/ (Code generated|@generated)/m.test(text.slice(0, 512))
  ) {
    return []
  }

  // Parse the file, reporting a syntax error as its only finding.
  const lines = text.split('\n')
  let program: Node
  try {
    program = parse(text, {
      sourceType: 'module',
      plugins: path.endsWith('x') ? ['typescript', 'jsx'] : ['typescript'],
    }).program
  } catch (err) {
    const loc = (err as { loc?: { line: number; column: number } }).loc
    const line = loc?.line ?? 1
    return [{ line, column: (loc?.column ?? 0) + 1, message: String(err) }]
  }

  // Check declared functions, methods and closures at every nesting level.
  const findings: Finding[] = []
  const visit = (node: Node): void => {
    if (functionTypes.has(node.type) && 'body' in node) {
      const body = node.body as Node
      if (body.type === 'BlockStatement') {
        checkBody(text, lines, body, findings)
      }
    }
    for (const child of children(node)) {
      visit(child)
    }
  }
  visit(program)
  return findings.sort((a, b) => a.line - b.line || a.column - b.column)
}

/** children returns a node's child nodes, skipping comments and positions. */
function children(node: Node): Node[] {
  const found: Node[] = []
  for (const [key, value] of Object.entries(node)) {
    if (key.endsWith('Comments') || key === 'loc' || key === 'extra') {
      continue
    }
    for (const item of Array.isArray(value) ? value : [value]) {
      if (item && typeof item === 'object' && 'type' in item) {
        found.push(item as Node)
      }
    }
  }
  return found
}

/** checkBody appends the paragraph violations of one function body. */
function checkBody(
  text: string,
  lines: string[],
  body: BlockStatement,
  findings: Finding[],
): void {
  // A condensed function is one paragraph under its declaration comment.
  if (body.body.length < paragraphMinStatements) {
    return
  }

  // Walk the statements, closing a paragraph at each blank line.
  let paragraph: Paragraph | undefined
  let prevEnd = body.loc!.start.line
  for (const statement of body.body) {
    // A purpose comment ends directly above its statement and starts below the previous one.
    const purposeStart = leadingPurpose(text, statement, prevEnd)
    const unitLine = purposeStart ?? statement.loc!.start.line

    // A blank line before the unit starts a paragraph; a purpose comment without one is misplaced.
    if (!paragraph || unitLine > prevEnd + 1) {
      if (paragraph) {
        reportParagraph(lines, paragraph, findings)
      }
      paragraph = { statements: [], purpose: purposeStart !== undefined }
    } else if (purposeStart !== undefined) {
      findings.push(
        finding(
          lines,
          purposeStart,
          'purpose comment must follow a blank line that ends the previous paragraph',
        ),
      )
    }
    paragraph.statements.push(statement)
    prevEnd = statement.loc!.end.line
  }
  if (paragraph) {
    reportParagraph(lines, paragraph, findings)
  }
}

/**
 * leadingPurpose returns the start line of the comment block that ends
 * directly above a statement and starts below the previous statement's end
 * line. Tool directives and TODO or XXX markers are not purpose comments.
 */
function leadingPurpose(
  text: string,
  statement: Statement,
  prevEnd: number,
): number | undefined {
  // Collect the comments on their own lines after the previous statement.
  const comments = (statement.leadingComments ?? []).filter(
    (comment) => comment.loc!.start.line > prevEnd,
  )

  // Walk up the contiguous block that ends on the line above the statement.
  let next = statement.loc!.start.line
  let start: number | undefined
  let purpose = false
  for (const comment of comments.toReversed()) {
    if (comment.loc!.end.line !== next - 1) {
      break
    }
    const source = text.slice(comment.start, comment.end)
    purpose ||= !directivePattern.test(source) && !markerPattern.test(source)
    start = comment.loc!.start.line
    next = start
  }
  return purpose ? start : undefined
}

/** reportParagraph appends a paragraph's missing comment or excess size. */
function reportParagraph(
  lines: string[],
  paragraph: Paragraph,
  findings: Finding[],
): void {
  // A lone return completes the action before it, and a test declaration is
  // named by its title; neither needs an outline entry.
  const first = paragraph.statements[0]!
  const line = first.loc!.start.line
  const soleReturn =
    paragraph.statements.length === 1 && first.type === 'ReturnStatement'
  if (
    !paragraph.purpose &&
    !soleReturn &&
    !paragraph.statements.every(isTestDeclaration)
  ) {
    findings.push(
      finding(
        lines,
        line,
        'code paragraph needs a purpose comment directly above it',
      ),
    )
  }

  // A paragraph past the limit holds more than one action.
  const count = paragraph.statements.length
  if (count > paragraphMaxStatements) {
    findings.push(
      finding(
        lines,
        line,
        `code paragraph has ${count} statements; split it into separate commented actions`,
      ),
    )
  }
}

/** isTestDeclaration reports whether a statement declares a test, suite or hook. */
function isTestDeclaration(statement: Statement): boolean {
  // Unwrap the call and any modifier chain such as test.skip or describe.each(table).
  if (
    statement.type !== 'ExpressionStatement' ||
    statement.expression.type !== 'CallExpression'
  ) {
    return false
  }
  let callee: Node = statement.expression.callee
  while (
    callee.type === 'MemberExpression' ||
    callee.type === 'CallExpression'
  ) {
    callee = callee.type === 'MemberExpression' ? callee.object : callee.callee
  }
  return callee.type === 'Identifier' && testCallees.has(callee.name)
}

/** finding builds a finding at the first non-blank column of a 1-based line. */
function finding(lines: string[], line: number, message: string): Finding {
  const column = (lines[line - 1] ?? '').search(/\S|$/) + 1
  return { line, column, message }
}

/** sourceFiles expands files and directories into handwritten TypeScript paths. */
function sourceFiles(paths: string[]): string[] {
  const files: string[] = []
  const walk = (path: string, explicit: boolean): void => {
    if (statSync(path).isFile()) {
      if (explicit || sourcePattern.test(path)) {
        files.push(path)
      }
      return
    }
    for (const entry of readdirSync(path).sort()) {
      if (!skippedDirs.has(entry) && !entry.startsWith('.')) {
        walk(join(path, entry), false)
      }
    }
  }
  for (const path of paths) {
    walk(path, true)
  }
  return files
}

/** main checks the named paths and exits with status 3 on violations. */
function main(): void {
  // Require at least one path to check.
  const paths = process.argv.slice(2)
  if (paths.length === 0) {
    console.error('usage: bun scripts/tsstyle.ts <file-or-directory>...')
    process.exit(2)
  }

  // Check every handwritten source file and print its findings.
  let count = 0
  for (const path of sourceFiles(paths)) {
    const text = readFileSync(path, 'utf8')
    for (const { line, column, message } of checkSource(path, text)) {
      console.log(`${path}:${line}:${column}: ${message}`)
      count++
    }
  }
  process.exit(count > 0 ? 3 : 0)
}

if (import.meta.main) {
  main()
}
