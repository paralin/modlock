// The library times the mod's handlers for the profiler of modlock dev and
// modlock play, which calls the "modlock.profile" service the library serves.
// The first call turns timing on; until then, and on a production server that
// never calls, each dispatch costs one test of profile.on and no clock read.
//
// Each handler is known by where the mod registered it: its kind, such as
// onFrame, and the bundle's line and column of the registering call, which
// the command line maps through the source map to the mod's own file.

/** profileService is the service the profiler calls. */
export const profileService = 'modlock.profile'

/** overMs is the time past which one call or frame counts as slow. */
export const overMs = 4

/** profile.on is true while the profiler reads the timings. */
export const profile = { on: false }

/** Site is one registered handler and its timings since the last read. */
export class Site {
  calls = 0
  /** total and max are milliseconds. */
  total = 0
  max = 0
  /** over counts the calls longer than overMs. */
  over = 0

  /** kind names the registration, such as onFrame; at is its bundle location. */
  constructor(
    readonly kind: string,
    readonly at: string,
  ) {}

  /** add counts one call that took ms. */
  add(ms: number): void {
    this.calls++
    this.total += ms
    if (ms > this.max) {
      this.max = ms
    }
    if (ms > overMs) {
      this.over++
    }
  }
}

/** sites holds every handler the mod registered, in order. */
const sites: Site[] = []

/** known finds each site by its kind and location, so a timer made each frame keeps one. */
const known = new Map<string, Site>()

/** frames holds the timings of whole frames since the last read. */
const frames = new Site('frame', '')

/** clock reads the runtime's millisecond clock. */
export function clock(): number {
  return performance.now()
}

/**
 * site records a handler of kind that the mod registers. Call it directly
 * from the function the mod called, so the stack's third frame is the mod's
 * call. A runtime without stacks, such as scriptc, leaves the location empty.
 */
export function site(kind: string): Site {
  return siteAt(kind, caller(new Error().stack ?? '', 2))
}

/** siteAt returns the site of kind at a location, made on first use. */
export function siteAt(kind: string, at: string): Site {
  // Reuse the site made for this kind and location, or make it.
  const key = `${kind} ${at}`
  const held = known.get(key)
  if (held) {
    return held
  }
  const made = new Site(kind, at)
  known.set(key, made)
  sites.push(made)
  return made
}

/**
 * caller returns the file:line:column of frame depth of a stack, such as
 * "mod.js:12:5", or empty. QuickJS writes "at f (file:1:2)" lines and
 * JavaScriptCore "f@file:1:2" lines, neither with a header line.
 */
export function caller(stack: string, depth: number): string {
  // Keep the frame lines, then take the location ending the one at depth.
  const lines = stack.split('\n').filter((line) => /:\d+:\d+\)?\s*$/.test(line))
  const line = lines[depth]
  if (line === undefined) {
    return ''
  }
  const match = /([^\s(@]+:\d+:\d+)\)?\s*$/.exec(line)
  return match?.[1] ?? ''
}

/** each calls every handler with event, timing each while profiling. */
export function each<E>(
  handlers: ((event: E) => void)[],
  at: Site[],
  event: E,
): void {
  if (!profile.on) {
    for (const handler of handlers) {
      handler(event)
    }
    return
  }
  let i = 0
  for (const handler of handlers) {
    const counted = at[i++]
    const start = clock()
    try {
      handler(event)
    } finally {
      counted?.add(clock() - start)
    }
  }
}

/**
 * eachWith calls call with every handler, timing each while profiling, for
 * handlers that take more than the event.
 */
export function eachWith<H>(
  handlers: H[],
  at: Site[],
  call: (handler: H) => void,
): void {
  if (!profile.on) {
    for (const handler of handlers) {
      call(handler)
    }
    return
  }
  let i = 0
  for (const handler of handlers) {
    const counted = at[i++]
    const start = clock()
    try {
      call(handler)
    } finally {
      counted?.add(clock() - start)
    }
  }
}

/** timed calls fn and counts its time to at. Call it only while profiling. */
export function timed(at: Site, fn: () => void): void {
  const start = clock()
  try {
    fn()
  } finally {
    at.add(clock() - start)
  }
}

/** frameStarted returns the clock at a frame's start, or 0 when not profiling. */
export function frameStarted(): number {
  return profile.on ? clock() : 0
}

/** frameEnded counts the frame that started at start, while profiling. */
export function frameEnded(start: number): void {
  if (profile.on && start !== 0) {
    frames.add(clock() - start)
  }
}

/**
 * read answers the profiler: method "read" turns timing on and returns the
 * timings since the last read as JSON, resetting them, and "stop" turns it
 * off.
 */
export function read(method: string): Uint8Array {
  // Stop turns timing off; any method but read is refused.
  if (method === 'stop') {
    profile.on = false
    return new Uint8Array()
  }
  if (method !== 'read') {
    throw new Error(`${profileService} has no method ${method}`)
  }
  const report = snapshot(profile.on)
  profile.on = true
  return new TextEncoder().encode(report)
}

/** snapshot writes the timings as JSON, and resets them when counted is true. */
function snapshot(counted: boolean): string {
  // Write the sites that ran, then start every count afresh.
  const rows: string[] = []
  if (counted) {
    for (const at of sites) {
      if (at.calls !== 0) {
        rows.push(row(at))
      }
    }
  }
  const text = `{"frames":${counted ? row(frames) : row(new Site('frame', ''))},"sites":[${rows.join(',')}]}`
  for (const at of sites) {
    reset(at)
  }
  reset(frames)
  return text
}

/** row writes one site's timings as a JSON object. */
function row(at: Site): string {
  return `{"kind":${JSON.stringify(at.kind)},"at":${JSON.stringify(at.at)},"calls":${at.calls},"ms":${at.total},"max":${at.max},"over":${at.over}}`
}

/** reset clears a site's timings. */
function reset(at: Site): void {
  at.calls = 0
  at.total = 0
  at.max = 0
  at.over = 0
}
