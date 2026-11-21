// Records keep one structured row per round, match or other event of a mod's
// game, of a kind the mod declares in mod.json, over the host's records
// service. The service answers at once and delivers later, so recording
// never waits. A host without the service, such as an older server, stores
// nothing; the mod logs each record instead, so its author sees what it
// would store.
//
//   record('round', { round: 3, area: 'docks', winner: 'seekers', seconds: 184.5 })
//   record('find', { round: 3, seeker, hider, prop: 'barrel' })

import * as recordsPb from '../../src/wasm/records/records.pb.js'
import { callService, log, Player } from './host.gen.js'

/** service names the host service that takes records. */
const service = 'records'

/** RecordValue is one field's value; undefined leaves the field out. */
export type RecordValue =
  | number
  | bigint
  | string
  | boolean
  | Player
  | undefined

/** RecordFields maps a record's field names to their values. */
export type RecordFields = Readonly<Record<string, RecordValue>>

/**
 * record adds one record of a kind the manifest declares. A whole number is
 * sent as an integer and any other number as a number; a Player is sent as
 * its slot, which the host stores as the player's SteamID. The host leaves
 * out a field the kind does not declare or whose value does not fit, and
 * logs it. It reports whether a records service took the record; a host
 * without one takes none, and the mod logs the record instead.
 */
export function record(kind: string, fields: RecordFields): boolean {
  // Encode each field that has a value.
  const values: Record<string, recordsPb.RecordValue> = {}
  for (const [name, value] of Object.entries(fields)) {
    const encoded = encode(value)
    if (encoded !== undefined) values[name] = encoded
  }

  // Hand the record to the host, or log it when no service takes it.
  const request = recordsPb.GameRecord.toBinary({ kind, fields: values })
  if (callService(service, 'Add', request) !== undefined) return true
  log(
    `record ${kind}: ${describe(fields)} (not stored: this host keeps no records)`,
  )
  return false
}

/** encode converts one field's value to the RecordValue the wire carries. */
function encode(value: RecordValue): recordsPb.RecordValue | undefined {
  switch (typeof value) {
    case 'number':
      return Number.isSafeInteger(value)
        ? { value: { case: 'integer', value: BigInt(value) } }
        : { value: { case: 'number', value } }
    case 'bigint':
      return { value: { case: 'integer', value } }
    case 'string':
      return { value: { case: 'text', value } }
    case 'boolean':
      return { value: { case: 'bool', value } }
    case 'object':
      return { value: { case: 'player', value: value.slot } }
  }
  return undefined
}

/** describe writes the fields in name order, such as "area=docks round=3". */
function describe(fields: RecordFields): string {
  return Object.keys(fields)
    .filter((name) => fields[name] !== undefined)
    .sort()
    .map((name) => {
      const value = fields[name]
      if (value instanceof Player) return `${name}=player ${value.slot}`
      if (typeof value === 'string') return `${name}=${JSON.stringify(value)}`
      return `${name}=${String(value)}`
    })
    .join(' ')
}
