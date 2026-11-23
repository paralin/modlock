// The host bridge: calls to the game and the events it delivers, through the
// runtime the mod runs on. Each runtime's entry binds its own transport, so
// the library itself reads no runtime globals.

import {
  Call,
  Reply,
  SubmitRequest,
  SubmitResponse,
} from '../../src/wasm/wasm.pb.js'

/** Transport exchanges an encoded Call for the host's encoded Reply. */
export type Transport = (call: Uint8Array) => Uint8Array

/** Dispatch answers one encoded Call the host delivers with its encoded Reply. */
export type Dispatch = (call: Uint8Array) => Uint8Array

/** unbound answers every call before a runtime binds the host as refused. */
const unbound: Transport = () =>
  Reply.toBinary({ error: 'the host is not bound yet' })

/** transport carries calls to the host; the runtime's entry binds it. */
let transport: Transport = unbound

/** dispatch delivers the host's events to the library's handlers. */
let dispatch: Dispatch | undefined

/** bind routes the mod's host calls through t from now on. */
export function bind(t: Transport): void {
  transport = t
}

/** serveEvents answers each event the host delivers with d. */
export function serveEvents(d: Dispatch): void {
  dispatch = d
}

/**
 * deliver answers one encoded Call from the host. The library registers its
 * dispatch when it loads, before any event can arrive.
 */
export function deliver(call: Uint8Array): Uint8Array {
  if (dispatch === undefined) {
    return Reply.toBinary({ error: 'the mod library is not loaded' })
  }
  return dispatch(call)
}

/** prefixes holds each method's encoded Call field 1, built on first use. */
const prefixes = new Map<string, Uint8Array>()

/**
 * methodPrefix encodes method as Call field 1. Method names are ASCII, so
 * each character is one byte; any other name takes the generated encoder.
 */
function methodPrefix(method: string): Uint8Array | undefined {
  // A method framed before reuses its prefix.
  const cached = prefixes.get(method)
  if (cached !== undefined) return cached

  // A name too long for a one byte length takes the generated encoder.
  const n = method.length
  if (n > 127) return undefined

  // The tag and length lead, then one byte per ASCII character.
  const prefix = new Uint8Array(2 + n)
  prefix[0] = 0x0a
  prefix[1] = n
  for (let i = 0; i < n; i++) {
    const c = method.charCodeAt(i)
    if (c > 0x7f) return undefined
    prefix[2 + i] = c
  }

  // Remember the prefix for the method's next call.
  prefixes.set(method, prefix)
  return prefix
}

/** varintSize returns the bytes a varint of value takes. */
function varintSize(value: number): number {
  let size = 1
  for (let rest = value; rest > 0x7f; rest >>>= 7) size++
  return size
}

/**
 * encodeCall frames method and request as a Call in one buffer. The
 * reflective encoder cost about 5 us a call in QuickJS, several times the
 * host crossing itself.
 */
function encodeCall(method: string, request: Uint8Array): Uint8Array {
  // A method without an ASCII prefix takes the generated encoder.
  const prefix = methodPrefix(method)
  if (prefix === undefined) return Call.toBinary({ method, request })

  // The method's prefix, then field 2's tag and length.
  const out = new Uint8Array(
    prefix.length + 1 + varintSize(request.length) + request.length,
  )
  out.set(prefix, 0)
  let at = prefix.length
  out[at++] = 0x12
  let size = request.length
  for (; size > 0x7f; size >>>= 7) out[at++] = (size & 0x7f) | 0x80
  out[at++] = size

  // The request's bytes close the Call.
  out.set(request, at)
  return out
}

/** empty is the response of a call that returned no bytes. */
const empty = new Uint8Array()

/**
 * decodeReply returns the Reply's response as a view into its bytes, or
 * undefined when the Reply carries an error. A field it cannot walk sends
 * the Reply to the generated decoder.
 */
function decodeReply(reply: Uint8Array): Uint8Array | undefined {
  let response: Uint8Array = empty
  let at = 0
  while (at < reply.length) {
    // A one byte tag covers both fields; anything longer is not ours.
    const tag = reply[at++] ?? 0xff
    if (tag > 0x7f) return fallbackReply(reply)

    // A varint follows: a length, or the value of a varint field.
    let value = 0
    let shift = 0
    for (;;) {
      if (at >= reply.length || shift > 28) return fallbackReply(reply)
      const b = reply[at++] ?? 0
      value += (b & 0x7f) * 2 ** shift
      shift += 7
      if (b < 0x80) break
    }

    // A varint field is skipped; only length-delimited fields carry bytes.
    const wire = tag & 7
    if (wire === 0) continue
    if (wire !== 2 || at + value > reply.length) return fallbackReply(reply)

    // A non-empty error fails the call; the response is viewed, not copied.
    const field = tag >>> 3
    if (field === 1 && value > 0) return undefined
    if (field === 2) response = reply.subarray(at, at + value)
    at += value
  }
  return response
}

/** fallbackReply decodes a Reply with the generated decoder. */
function fallbackReply(bytes: Uint8Array): Uint8Array | undefined {
  const reply = Reply.fromBinary(bytes)
  return reply.error ? undefined : (reply.response ?? empty)
}

/**
 * call calls one Host method with its encoded request and returns the
 * encoded response, or undefined when the call failed; the host logs why.
 * The response may be a view into the reply the transport returned.
 */
export function call(
  method: string,
  request: Uint8Array,
): Uint8Array | undefined {
  // The calls asked for without waiting went first, so each call sees the
  // calls the mod made before it.
  if (unsent.length > 0) flushUnsent()
  return decodeReply(transport(encodeCall(method, request)))
}

/** Unsent is one call asked for without waiting, and how its result settles. */
interface Unsent {
  readonly method: string
  readonly request: Uint8Array
  readonly settle: (response: Uint8Array | undefined) => void
}

/**
 * unsent holds the calls callAsync took since the last flush. It lives at
 * module scope, as does sent, because the runtime binds the transport anew for
 * each event, and a promise outlives the event that made it.
 */
let unsent: Unsent[] = []

/** sent maps the number of each submitted call to the call awaiting its reply. */
const sent = new Map<bigint, Unsent>()

/** flushScheduled is true while a flush waits for the end of the turn. */
let flushScheduled = false

/** submitRefused is true once the host answered that it has no Submit. */
let submitRefused = false

/**
 * callAsync is call without the wait: the promise settles with the response, or
 * with undefined when the call failed, in a frame after the call. The calls a
 * mod asks for in one turn, which Promise.all makes together, go to the host
 * in one Submit. A host that does not know Submit, or refuses the batch, runs
 * each call as call would, so the promise settles all the same.
 */
export function callAsync(
  method: string,
  request: Uint8Array,
): Promise<Uint8Array | undefined> {
  return new Promise((settle) => {
    unsent.push({ method, request, settle })

    // The turn's other calls join this one; the batch goes when it ends.
    if (!flushScheduled) {
      flushScheduled = true
      void Promise.resolve().then(flushUnsent)
    }
  })
}

/**
 * completeCalls settles the submitted calls the host's Completed event
 * delivers, in the order they were submitted. Their continuations run when
 * the runtime next drains its promise jobs, which for QuickJS is before the
 * event returns.
 */
export function completeCalls(event: {
  readonly calls: readonly {
    readonly call: bigint
    readonly reply: { readonly error: string; readonly response: Uint8Array }
  }[]
}): void {
  for (const done of event.calls) {
    // A call settles once; a number this instance does not hold is ignored.
    const waiting = sent.get(done.call)
    if (waiting === undefined) continue
    sent.delete(done.call)
    const reply = done.reply
    waiting.settle(reply.error ? undefined : reply.response)
  }
}

/** flushUnsent sends the calls callAsync took, in one Submit when it can. */
function flushUnsent(): void {
  // The turn ended, so the calls it took are all there are.
  flushScheduled = false
  if (unsent.length === 0) return

  // Take the batch, so a call made while it runs joins the next one.
  const batch = unsent
  unsent = []

  // The host numbers the calls and answers them in a later frame.
  const numbers = submitBatch(batch)
  if (numbers !== undefined) {
    for (const [i, waiting] of batch.entries()) {
      sent.set(numbers[i] ?? 0n, waiting)
    }
    return
  }

  // Without Submit each call runs now, as it would have synchronously.
  for (const { method, request, settle } of batch) {
    settle(call(method, request))
  }
}

/**
 * submitBatch starts batch's calls and returns their numbers, or undefined when
 * the host did not take the batch. A host with no Submit is not asked again.
 */
function submitBatch(batch: readonly Unsent[]): bigint[] | undefined {
  // A host that lacks Submit stays refused.
  if (submitRefused) return undefined

  // Ask the host to start the calls.
  const request = SubmitRequest.toBinary({
    calls: batch.map(({ method, request }) => ({ method, request })),
  })
  const reply = Reply.fromBinary(transport(encodeCall('Submit', request)))
  if (reply.error) {
    // An older host answers Submit as it answers any name it lacks.
    submitRefused = reply.error.includes('has no method')
    return undefined
  }

  // The host numbers each call, one for each in order.
  const numbers = SubmitResponse.fromBinary(reply.response ?? empty).calls ?? []
  return numbers.length === batch.length ? numbers : undefined
}
