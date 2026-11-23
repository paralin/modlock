// The host bridge: calls to the game and the events it delivers, through the
// runtime the mod runs on. Each runtime's entry binds its own transport, so
// the library itself reads no runtime globals.

import { Call, Reply } from '../../src/wasm/wasm.pb.js'

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
 * revision counts the moments the game may have changed under the mod: each
 * event the host delivers, since the game runs between events, and each call
 * that is not read-only, since the mod itself may have changed the game. Two
 * reads made at the same revision inside one event see the same game, so a
 * reader may keep what it read until the revision moves.
 */
let revision = 0

/** inEvent is how many deliveries are running, nested ones included. */
let inEvent = 0

/**
 * gameRevision returns the current revision while an event is running, or
 * undefined outside one, where the game may change between any two calls
 * and nothing read may be kept.
 */
export function gameRevision(): number | undefined {
  return inEvent > 0 ? revision : undefined
}

/**
 * deliver answers one encoded Call from the host. The library registers its
 * dispatch when it loads, before any event can arrive.
 */
export function deliver(call: Uint8Array): Uint8Array {
  // Before the library loads, nothing can answer.
  if (dispatch === undefined) {
    return Reply.toBinary({ error: 'the mod library is not loaded' })
  }

  // The game ran since the last event and runs again after this one. An
  // event whose handler throws fails as a whole, and the next event starts
  // from a new revision all the same.
  const depth = inEvent
  revision++
  inEvent = depth + 1
  const reply = dispatch(call)
  inEvent = depth
  revision++
  return reply
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
 * The response may be a view into the reply the transport returned. The call
 * may change the game, so it moves the revision.
 */
export function call(
  method: string,
  request: Uint8Array,
): Uint8Array | undefined {
  const reply = transport(encodeCall(method, request))
  revision++
  return decodeReply(reply)
}

/**
 * callRead calls a Host method the schema marks NO_SIDE_EFFECTS, as call
 * does, but leaves the revision where it is: the game is the same after it.
 */
export function callRead(
  method: string,
  request: Uint8Array,
): Uint8Array | undefined {
  return decodeReply(transport(encodeCall(method, request)))
}
