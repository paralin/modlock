// Persist keeps records for each player across sessions, such as a player's
// stats or unlocks, in the record types a mod declares in mod.json under
// persist, over the host's persist service. A read starts with the host and
// answers loading until the record arrives a frame or more later; an update
// sends field changes the host applies whole, so two servers that update one
// player at once both land.
//
//   const stats = persist.get(player, 'stats')
//   if (stats.state === 'loaded') log('rounds won', stats.record.roundsWon ?? 0)
//   persist.update(player, 'stats', { roundsWon: { add: 1 } }, { key: roundId })

import * as persistPb from '../../src/wasm/persist/persist.pb.js'
import { callServiceLater } from './service-later.js'
import { log, type Player } from './host.gen.js'

/** service names the host service that keeps records. */
const service = 'persist'

/** PersistValue is one field's value: a number, a string or a tags field's members. */
export type PersistValue = number | string | readonly string[]

/** PersistRecord is one player's record: each field written so far, by name. */
export type PersistRecord = Readonly<Record<string, PersistValue>>

/**
 * PersistRead is what persist.get knows of a record: loading until the host
 * answers, then loaded, or unavailable with why, such as a host that keeps
 * no records.
 */
export type PersistRead =
  | { readonly state: 'loading' }
  | { readonly state: 'loaded'; readonly record: PersistRecord }
  | { readonly state: 'unavailable'; readonly error: string }

/**
 * PersistChange is one change to one field: add to a count or number, max
 * or min a number, set a count, number or string, insert or remove a tag.
 */
export type PersistChange =
  | { readonly add: number }
  | { readonly max: number }
  | { readonly min: number }
  | { readonly set: number | string }
  | { readonly insert: string }
  | { readonly remove: string }

/** PersistChanges names the changes of one update by field, one or several each. */
export type PersistChanges = Readonly<
  Record<string, PersistChange | readonly PersistChange[]>
>

/** PersistOptions describe one update. */
export interface PersistOptions {
  /**
   * key identifies the change, such as a round, so a resent update counts
   * once. An update that adds needs one.
   */
  key?: string
  /** record selects the record; leave it unset, as hosts keep one record of each type for now. */
  record?: string
}

/**
 * PersistDone ends one update: with the record after it, and whether its key
 * made it a repeat the host left out, or with the error that refused it.
 */
export type PersistDone = (
  record: PersistRecord | undefined,
  error?: string,
  repeated?: boolean,
) => void

/** Entry is what the mod knows of one player's record of one type. */
interface Entry {
  read: PersistRead
  /** started numbers the last request started for the record. */
  started: number
  /** applied numbers the request whose answer read holds. */
  applied: number
}

/**
 * entries holds each player's records by type. A Player is a new object when
 * a new connection takes the slot, so a record never passes to the next
 * player in a slot.
 */
const entries = new WeakMap<Player, Map<string, Entry>>()

/** requests numbers the requests the mod started, in order. */
let requests = 0

/** entry returns the record of type for player, made empty when the mod knows none. */
function entry(player: Player, type: string): Entry {
  // Find the player's records, then the type's, making each when missing.
  let records = entries.get(player)
  if (!records) {
    records = new Map()
    entries.set(player, records)
  }
  let known = records.get(type)
  if (!known) {
    known = { read: { state: 'loading' }, started: 0, applied: 0 }
    records.set(type, known)
  }
  return known
}

/**
 * settle keeps an answer to the request numbered request, unless the answer
 * to a later one already arrived.
 */
function settle(known: Entry, request: number, read: PersistRead): void {
  if (request < known.applied) return
  known.applied = request
  known.read = read
}

/** start numbers a new request for known. */
function start(known: Entry): number {
  known.started = ++requests
  return known.started
}

/** load asks the host for player's record of type. */
function load(player: Player, type: string, known: Entry): void {
  const request = start(known)
  callServiceLater(
    service,
    'Get',
    persistPb.GetRequest.toBinary({ player: player.slot, type }),
    (reply, error) => {
      if (reply === undefined) {
        settle(known, request, unavailable(type, error))
        return
      }
      const answer = persistPb.GetReply.fromBinary(reply)
      settle(known, request, { state: 'loaded', record: decode(answer.record) })
    },
  )
}

/** unavailable is the read of a record the host did not answer, logged once. */
function unavailable(type: string, error: string | undefined): PersistRead {
  const why = error || 'the host answered nothing'
  log(`persist ${type}: ${why}`)
  return { state: 'unavailable', error: why }
}

export const persist = {
  /**
   * get returns player's record of type, a record type the manifest
   * declares. The first get starts the read and answers loading; a later
   * get answers what the host sent. A record never written loads empty.
   */
  get(player: Player, type: string): PersistRead {
    const known = entry(player, type)
    if (known.started === 0) load(player, type, known)
    return known.read
  },

  /**
   * getMany starts reading the record of type for each of players, up to
   * 16, in one call, such as everyone in a lobby at join. persist.get then
   * answers each.
   */
  getMany(players: readonly Player[], type: string): void {
    const pending = players.map((player) => {
      const known = entry(player, type)
      return { player, known, request: start(known) }
    })
    callServiceLater(
      service,
      'GetMany',
      persistPb.GetManyRequest.toBinary({
        players: players.map((player) => player.slot),
        type,
      }),
      (reply, error) => {
        if (reply === undefined) {
          const read = unavailable(type, error)
          for (const p of pending) settle(p.known, p.request, read)
          return
        }
        const records = new Map(
          (persistPb.GetManyReply.fromBinary(reply).records ?? []).map((r) => [
            r.player ?? 0,
            decode(r.record),
          ]),
        )
        for (const p of pending) {
          settle(p.known, p.request, {
            state: 'loaded',
            record: records.get(p.player.slot) ?? {},
          })
        }
      },
    )
  },

  /**
   * update changes player's record of type by changes, which the host
   * applies all or none. With a key, an update the host already took is left
   * out, so resending after a lost answer counts once. An update with an add
   * and no key is refused without being sent, since a resend would count it
   * twice. persist.get answers the changed record once the host has
   * answered, and done runs then.
   */
  update(
    player: Player,
    type: string,
    changes: PersistChanges,
    options: PersistOptions = {},
    done?: PersistDone,
  ): void {
    // An add needs a key, as the host requires.
    const ops = encode(changes)
    if (!options.key && ops.some((op) => op.op?.case === 'add')) {
      const why =
        'a persist update that adds needs an update key, so a resend counts once'
      log(`persist ${type} update: ${why}`)
      done?.(undefined, why)
      return
    }
    const known = entry(player, type)
    const request = start(known)
    callServiceLater(
      service,
      'Update',
      persistPb.UpdateRequest.toBinary({
        player: player.slot,
        type,
        key: options.record,
        ops,
        updateKey: options.key,
      }),
      (reply, error) => {
        // A refused update leaves the record as get last read it.
        if (reply === undefined) {
          const why = error || 'the host answered nothing'
          log(`persist ${type} update: ${why}`)
          done?.(undefined, why)
          return
        }
        const answer = persistPb.UpdateReply.fromBinary(reply)
        const record = decode(answer.record)
        settle(known, request, { state: 'loaded', record })
        done?.(record, undefined, answer.repeated ?? false)
      },
    )
  },

  /** refresh reads player's record of type again, such as after unavailable. */
  refresh(player: Player, type: string): void {
    load(player, type, entry(player, type))
  },
}

/** encode converts changes to the host's operations, in field order. */
function encode(changes: PersistChanges): persistPb.Op[] {
  const ops: persistPb.Op[] = []
  for (const [field, change] of Object.entries(changes)) {
    const list: readonly PersistChange[] = Array.isArray(change)
      ? change
      : [change as PersistChange]
    for (const one of list) ops.push({ field, op: operation(one) })
  }
  return ops
}

/** operation converts one change to an operation. */
function operation(change: PersistChange): persistPb.Op['op'] {
  // Each change names one operation; a set holds a number or text.
  if ('add' in change) return { case: 'add', value: change.add }
  if ('max' in change) return { case: 'max', value: change.max }
  if ('min' in change) return { case: 'min', value: change.min }
  if ('insert' in change) return { case: 'insert', value: change.insert }
  if ('remove' in change) return { case: 'remove', value: change.remove }
  const value: persistPb.Value['value'] =
    typeof change.set === 'string'
      ? { case: 'text', value: change.set }
      : { case: 'number', value: change.set }
  return { case: 'set', value: { value } }
}

/** decode converts a record the host sent to a PersistRecord. */
function decode(record: persistPb.Record | undefined): PersistRecord {
  const fields: Record<string, PersistValue> = {}
  for (const [name, value] of Object.entries(record?.fields ?? {})) {
    const v = value.value
    if (v?.case === 'number' || v?.case === 'text') fields[name] = v.value
    if (v?.case === 'tags') fields[name] = v.value.members ?? []
  }
  return fields
}
