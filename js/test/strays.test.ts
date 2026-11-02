import { expect, test } from 'bun:test'

import { HoldModifierStateRequest } from '../../src/wasm/abilities/abilities.pb.js'
import { HitOptions } from '../../src/wasm/combat/combat.pb.js'
import { EntitiesResponse } from '../../src/wasm/entities/entities.pb.js'
import {
  FieldResponse,
  ReadFieldRequest,
  WriteFieldRequest,
} from '../../src/wasm/fields/fields.pb.js'
import { Call, Reply } from '../../src/wasm/wasm.pb.js'
import { bind } from '../src/host.js'
import { keepPlayers } from '../src/players.js'
import { Strays } from '../src/strays.js'

// The transport supplies native fields; Strays owns the abandonment decision.
test('disconnected controllers do not retain abandoned heroes while joining heroes stay protected', () => {
  // Six controllers retain their hero handle across their connection states.
  const states = [0, 1, 2, 3, 4, undefined]
  const hits: number[] = []
  const held: HoldModifierStateRequest[] = []
  const respawns: number[] = []
  keepPlayers([])
  bind((bytes) => {
    const call = Call.fromBinary(bytes)
    let response: Uint8Array = new Uint8Array()
    switch (call.method) {
      case 'FindEntities':
        response = EntitiesResponse.toBinary({
          entities: states.map((_, i) => i + 1),
        })
        break
      case 'ReadField': {
        const r = ReadFieldRequest.fromBinary(call.request)
        const entity = r.entity ?? 0
        const value =
          r.field === 'm_hController'
            ? entity + 100
            : r.field === 'm_hHeroPawn'
              ? entity - 100
              : r.field === 'm_iConnected'
                ? states[entity - 101]
                : r.field === 'm_iHealth'
                  ? 100
                  : undefined
        response = FieldResponse.toBinary(
          value === undefined
            ? {}
            : { value: { value: { case: 'number', value } } },
        )
        break
      }
      case 'Damage':
        hits.push(HitOptions.fromBinary(call.request).victim ?? 0)
        break
      case 'HoldModifierState':
        held.push(HoldModifierStateRequest.fromBinary(call.request))
        break
      case 'WriteField':
        respawns.push(WriteFieldRequest.fromBinary(call.request).entity ?? 0)
        break
    }
    return Reply.toBinary({ response })
  })

  // Nothing is removed during the grace period, then only disconnected heroes.
  const strays = new Strays()
  strays.sweep(0)
  strays.sweep(2)
  expect(hits).toEqual([])
  strays.sweep(3)
  expect(hits).toEqual([4, 5])
  expect(respawns).toEqual([4, 5])
  expect(
    held.filter((r) => r.state === 'MODIFIER_STATE_MELEE_DISABLED'),
  ).toEqual([
    { entity: 4, state: 'MODIFIER_STATE_MELEE_DISABLED', active: false },
    { entity: 5, state: 'MODIFIER_STATE_MELEE_DISABLED', active: false },
  ])

  // A controller that reconnects during the grace period retains its hero.
  hits.length = 0
  states[3] = 2
  strays.sweep(4)
  expect(hits).toEqual([5])
})
