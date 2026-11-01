// The part of bun:test the library's tests use, written for a scriptc build:
// the suite's configuration maps bun:test here, so the same test files run on
// Bun and as WebAssembly. A passing case prints "ok" and a failing one "FAIL"
// with its reason, one line each.

/** Case is one registered test. */
interface Case {
  name: string
  body: () => void
}

const cases: Case[] = []
let scope = ''

/** asserted counts the expectations the running test has made, so a throw
 * from code under test is located by the expectation before it. */
let asserted = 0

/** describe names the tests body registers. */
export function describe(name: string, body: () => void): void {
  const outer = scope
  scope = outer + name + ' > '
  body()
  scope = outer
}

/** test registers one test. */
export function test(name: string, body: () => void): void {
  cases.push({ name: scope + name, body })
}

/** it is test. */
export const it = test

/** same reports whether a and b are the same JSON value. */
function same(a: unknown, b: unknown): boolean {
  return json(a) === json(b)
}

/** json writes value as JSON, a bigint as its digits. */
function json(value: unknown): string {
  return JSON.stringify(value, (_, v: unknown) =>
    typeof v === 'bigint' ? String(v) : v,
  )
}

/** Expectation is what a test asserts about one value. */
class Expectation {
  readonly actual: unknown
  readonly negated: boolean

  constructor(actual: unknown, negated: boolean) {
    this.actual = actual
    this.negated = negated
  }

  /** not inverts the next assertion. */
  get not(): Expectation {
    return new Expectation(this.actual, !this.negated)
  }

  /** check fails the test unless holds matches the expectation's sense. */
  private check(holds: boolean, what: string): void {
    if (holds === this.negated) {
      throw new Error(
        `expected ${json(this.actual)} ${this.negated ? 'not ' : ''}${what}`,
      )
    }
  }

  toBe(want: unknown): void {
    this.check(this.actual === want, `to be ${json(want)}`)
  }

  toEqual(want: unknown): void {
    this.check(same(this.actual, want), `to equal ${json(want)}`)
  }

  toBeUndefined(): void {
    this.check(this.actual === undefined, 'to be undefined')
  }

  toBeDefined(): void {
    this.check(this.actual !== undefined, 'to be defined')
  }

  toBeNull(): void {
    this.check(this.actual === null, 'to be null')
  }

  toBeInstanceOf(type: abstract new (...args: never[]) => unknown): void {
    this.check(this.actual instanceof type, 'to be an instance')
  }

  toBeGreaterThan(than: number): void {
    this.check((this.actual as number) > than, `to be above ${than}`)
  }

  toBeCloseTo(want: number): void {
    const gap = Math.abs((this.actual as number) - want)
    this.check(gap < 0.005, `to be close to ${want}`)
  }

  toHaveLength(length: number): void {
    const got = (this.actual as unknown[]).length
    this.check(got === length, `to have length ${length}`)
  }

  toMatchObject(want: unknown): void {
    this.check(matches(this.actual, want), `to match ${json(want)}`)
  }
}

/** matches reports whether actual holds every field of want, deeply. */
function matches(actual: unknown, want: unknown): boolean {
  // Compare a scalar or an array whole.
  if (typeof want !== 'object' || want === null || Array.isArray(want)) {
    return same(actual, want)
  }
  if (typeof actual !== 'object' || actual === null) return false

  // Match each field of want.
  const have = actual as Record<string, unknown>
  for (const [key, value] of Object.entries(want as Record<string, unknown>)) {
    if (!matches(have[key], value)) return false
  }
  return true
}

/** expect starts an assertion about actual. */
export function expect(actual: unknown): Expectation {
  asserted++
  return new Expectation(actual, false)
}

/** run runs every registered test and prints each result. */
export function run(): void {
  let failed = 0
  for (const c of cases) {
    asserted = 0
    try {
      c.body()
      console.log(`ok ${c.name}`)
    } catch (e) {
      failed++
      console.log(
        `FAIL ${c.name}: ${(e as Error).message} (expectation ${asserted})`,
      )
    }
  }
  console.log(`${cases.length - failed} passed, ${failed} failed`)
}
