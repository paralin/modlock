// The per-event pawn read behind Player.pawn(). A mode whose systems each read
// every player's pawn on each frame would otherwise pay a Pawn call and a full
// decode for every read, though the game cannot change between two reads in
// one event unless the mod itself changed it. This module answers those reads
// from what the event already read:
//
//   - The host's revision (host.ts) moves with each event and with each call
//     the schema does not mark NO_SIDE_EFFECTS. A pawn read at the current
//     revision is answered again without a call.
//   - The first slot read at a revision takes its own Pawn call. A second
//     slot read at the same revision reads every slot with one Pawns call,
//     which answers the rest of the revision's reads.
//   - Each slot keeps its last decoded Pawn and the bytes it came from. A
//     slot whose bytes have not changed gets the same Pawn back without a
//     decode, as a hero standing still does frame after frame.
//
// A kept Pawn is shared by every reader that receives it, so it must not be
// changed; its fields are readonly. Outside an event, and while
// setPawnCache(false) holds, every read is a Pawn call and a decode.

import { PlayerRequest } from '../../src/wasm/players/players.pb.js'
import type { Pawn } from './host.gen.js'
import { callRead, gameEvent, gameRevision } from './host.js'

/** PawnDecoder turns one slot's encoded Pawn into the library's Pawn. */
export type PawnDecoder = (bytes: Uint8Array) => Pawn

/** Slot is what one slot's pawn read last found. */
interface Slot {
  /** revision is the revision at which the slot was last read. */
  revision: number
  /** bytes is the slot's encoded Pawn, or undefined when it had none. */
  bytes: Uint8Array | undefined
  /** pawn is bytes decoded. */
  pawn: Pawn | undefined
}

/** cacheOn is setPawnCache's switch. */
let cacheOn = true

/** slots holds each slot's last read, by slot. */
const slots = new Map<number, Slot>()

/** singleRevision is the revision of the last Pawn call for one slot. */
let singleRevision = -1

/** batchRevision is the revision batch was read at. */
let batchRevision = -1

/** batch holds each slot's encoded Pawn from the last Pawns call. */
const batch = new Map<number, Uint8Array>()

/**
 * batchFailed is the event in which the host last answered Pawns with an
 * error. The rest of that event reads each slot alone, and the next event
 * tries Pawns again, so a passing error costs one event its batch.
 */
let batchFailed = -1

/**
 * setPawnCache turns the per-event pawn read on, as it starts, or off, when
 * every Player.pawn() makes its own Pawn call and decode, so a mode can
 * compare the two. Either way the next read starts afresh.
 */
export function setPawnCache(on: boolean): void {
  // Forget every read, so the switch takes effect at the next one.
  cacheOn = on
  slots.clear()
  batch.clear()
  singleRevision = -1
  batchRevision = -1
  batchFailed = -1
}

/**
 * pawnOf answers Player.pawn() for slot, decoding a Pawn's bytes with
 * decode.
 */
export function pawnOf(slot: number, decode: PawnDecoder): Pawn | undefined {
  // Outside an event, or with the cache off, every read reaches the host.
  const now = gameRevision()
  if (!cacheOn || now === undefined) {
    const bytes = readOne(slot)
    return bytes === undefined ? undefined : decode(bytes)
  }

  // A slot read at this revision is answered again.
  const kept = slots.get(slot)
  if (kept !== undefined && kept.revision === now) return kept.pawn

  // Read the slot alone first, and every slot once a second is read. The
  // batch is a copy of its own; a single reply is copied when kept.
  let bytes: Uint8Array | undefined
  let fresh = false
  if (
    batchRevision !== now &&
    singleRevision === now &&
    batchFailed !== gameEvent()
  ) {
    readBatch(now)
  }
  if (batchRevision === now) {
    bytes = batch.get(slot)
  } else {
    singleRevision = now
    bytes = readOne(slot)
    fresh = true
  }

  // Bytes the slot's last decode came from give back the same Pawn.
  let pawn: Pawn | undefined
  if (kept !== undefined && sameBytes(kept.bytes, bytes)) {
    pawn = kept.pawn
    bytes = kept.bytes
  } else if (bytes !== undefined) {
    if (fresh) bytes = copyOf(bytes)
    pawn = decode(bytes)
  }
  slots.set(slot, { revision: now, bytes, pawn })
  return pawn
}

/**
 * readOne returns slot's encoded Pawn from its own Pawn call, as a view into
 * the reply, or undefined when the slot has none, the call failed or the
 * reply is unreadable.
 */
function readOne(slot: number): Uint8Array | undefined {
  // Ask the host for the one slot.
  const reply = callRead('Pawn', PlayerRequest.toBinary({ player: slot }))
  if (reply === undefined) return undefined

  // The PawnResponse holds the Pawn as its field 1.
  const fields = new Fields(reply)
  let pawn: Uint8Array | undefined
  while (fields.next()) {
    if (fields.field === 1 && fields.bytes !== undefined) pawn = fields.bytes
  }
  return fields.bad ? undefined : pawn
}

/**
 * readBatch fills batch from one Pawns call at revision now. Its response is
 * copied once, so the slots' views outlive the transport's next reply. A
 * host that answers with an error, such as one older than Pawns, leaves the
 * rest of the event's reads to their own calls; a reply the walk cannot read
 * leaves this revision's.
 */
function readBatch(now: number): void {
  // Ask the host for every slot.
  const reply = callRead('Pawns', new Uint8Array())
  if (reply === undefined) {
    batchFailed = gameEvent()
    return
  }

  // Each PlayerPawn entry holds its slot as field 1 and its Pawn as field 2.
  batch.clear()
  const entries = new Fields(copyOf(reply))
  while (entries.next()) {
    if (entries.field !== 1 || entries.bytes === undefined) continue
    const entry = new Fields(entries.bytes)
    let player = 0
    let pawn: Uint8Array | undefined
    while (entry.next()) {
      if (entry.field === 1) player = entry.value
      if (entry.field === 2 && entry.bytes !== undefined) pawn = entry.bytes
    }
    if (entry.bad) return
    if (pawn !== undefined) batch.set(player, pawn)
  }
  if (entries.bad) return
  batchRevision = now
}

/**
 * Fields walks the top-level fields of one encoded message. Each next()
 * stops at a field: a varint's value, or a length-delimited field's bytes as
 * a view. It reads every wire type proto3 writes, so only damaged bytes,
 * such as a field running past the end, end it with bad set.
 */
class Fields {
  /** field is the current field's number. */
  field = 0
  /** value is the current varint field's value. */
  value = 0
  /** bytes is the current length-delimited field's bytes. */
  bytes: Uint8Array | undefined = undefined
  /** bad is set when the walk met bytes it cannot read. */
  bad = false
  /** message is the message walked. */
  private readonly message: Uint8Array
  /** at is where the next field starts. */
  private at = 0

  constructor(message: Uint8Array) {
    this.message = message
  }

  /** next moves to the next field, returning false at the end. */
  next(): boolean {
    // The tag names the field and its wire type.
    if (this.at >= this.message.length) return false
    const tag = this.varint()
    if (tag === undefined) return this.fail()
    this.field = Math.floor(tag / 8)
    this.bytes = undefined
    const wire = tag % 8

    // A varint field carries its value.
    if (wire === 0) {
      const value = this.varint()
      if (value === undefined) return this.fail()
      this.value = value
      return true
    }

    // A length-delimited field carries its bytes, viewed in place.
    if (wire === 2) {
      const size = this.varint()
      if (size === undefined || this.at + size > this.message.length) {
        return this.fail()
      }
      this.bytes = this.message.subarray(this.at, this.at + size)
      this.at += size
      return true
    }

    // A fixed-width field is skipped; a group, which proto3 never writes,
    // ends the walk.
    const skip = wire === 1 ? 8 : wire === 5 ? 4 : -1
    if (skip < 0 || this.at + skip > this.message.length) return this.fail()
    this.at += skip
    return true
  }

  /** varint reads one varint, or undefined past the end or beyond 2^53. */
  private varint(): number | undefined {
    let value = 0
    let scale = 1
    while (this.at < this.message.length && scale < maxScale) {
      const b = this.message[this.at++] ?? 0
      value += (b & 0x7f) * scale
      if (b < 0x80) return value
      scale *= 128
    }
    return undefined
  }

  /** fail ends the walk as unreadable. */
  private fail(): boolean {
    this.bad = true
    this.at = this.message.length
    return false
  }
}

/** maxScale is 2^53, past which a varint's bits no longer fit a number. */
const maxScale = 9007199254740992

/** sameBytes reports whether a and b hold the same bytes, or are both absent. */
function sameBytes(
  a: Uint8Array | undefined,
  b: Uint8Array | undefined,
): boolean {
  if (a === undefined || b === undefined) return a === b
  if (a.length !== b.length) return false
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false
  return true
}

/** copyOf returns bytes in a buffer of their own. */
function copyOf(bytes: Uint8Array): Uint8Array {
  const out = new Uint8Array(bytes.length)
  out.set(bytes, 0)
  return out
}
