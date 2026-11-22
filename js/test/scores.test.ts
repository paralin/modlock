import { expect, test } from 'bun:test'

import {
  BoardPage as PageMessage,
  BoardQuery,
  MatchReport,
  ScoreConnection,
  ScoreReport,
} from '../../src/wasm/scores/scores.pb.js'
import {
  Call,
  Reply,
  ServiceCall,
  ServiceCallStarted,
  ServiceReply,
  StartServiceCallRequest,
} from '../../src/wasm/wasm.pb.js'
import { Player, type Connection } from '../src/host.gen.js'
import { bind } from '../src/host.js'
import {
  bindScorePlayer,
  readBoard,
  reportMatch,
  reportScore,
  type BoardPage,
  type ScoreDone,
} from '../src/scores.js'
import { endServiceCall } from '../src/service-later.js'

/** player is the enrolled human in slot zero. */
const player: Connection = {
  player: Player.of(0),
  steamId: 76561198000000002n,
  generation: 5,
  bot: false,
  ready: true,
  name: 'Jun',
  away: false,
  roles: [],
}

/** sent retains each started score request for the wire assertions. */
const sent: StartServiceCallRequest[] = []
bind((bytes) => {
  // Bind returns proof for the identity it saw.
  const request = Call.fromBinary(bytes)
  if (request.method === 'CallService') {
    const bind = ServiceCall.fromBinary(request.request)
    expect([bind.service, bind.method]).toEqual(['scores', 'Bind'])
    const bound = ScoreConnection.fromBinary(bind.payload)
    return Reply.toBinary({
      response: ServiceReply.toBinary({
        payload: ScoreConnection.toBinary({ ...bound, proof: 'test proof' }),
      }),
    })
  }

  // The report and read return only a call number until the frame arrives.
  expect(request.method).toBe('StartServiceCall')
  sent.push(StartServiceCallRequest.fromBinary(request.request))
  return Reply.toBinary({
    response: ServiceCallStarted.toBinary({ call: BigInt(sent.length) }),
  })
})

test('a lost score reply can resend the identical enrolled result', () => {
  // Submit a result and wait for the backend, even with slot zero.
  const enrolled = bindScorePlayer(player)!
  const result: {
    calls: number
    error?: string
    retry?: (done: ScoreDone) => void
  } = { calls: 0 }
  reportScore(
    enrolled,
    'trial.time',
    41250.4,
    (error, retry) => {
      result.calls++
      result.error = error
      result.retry = retry
    },
    {
      partition: { course: 'canyon' },
      key: 'run-7',
    },
  )
  const first = sent.at(-1)!
  expect([first.service, first.method]).toEqual(['scores', 'Report'])
  expect(ScoreReport.fromBinary(first.payload)).toMatchObject({
    value: 41250n,
    partition: { course: 'canyon' },
    key: 'run-7',
    connection: { generation: 5, steamId: player.steamId, proof: 'test proof' },
  })
  expect(result.calls).toBe(0)

  // Retrying after a timeout sends exactly the same bytes.
  endServiceCall({
    call: BigInt(sent.length),
    failure: 'timeout',
    error: 'The reply was lost',
    payload: new Uint8Array(),
  })
  expect(result.error).toBe('The reply was lost')
  result.retry!((error) => {
    result.calls++
    result.error = error
  })
  expect(sent.at(-1)?.payload).toEqual(first.payload)
  endServiceCall({
    call: BigInt(sent.length),
    payload: new Uint8Array(),
    failure: 'none',
    error: '',
  })
  expect(result.calls).toBe(2)
  expect(result.error).toBeUndefined()
})

test('a match and a viewer read use the same asynchronous service', () => {
  // The match carries the participant proof with its placement.
  const enrolled = bindScorePlayer(player)!
  reportMatch('cup', [{ player: enrolled, place: 1 }], () => {})
  const match = sent.at(-1)!
  expect(match.method).toBe('Match')
  expect(
    MatchReport.fromBinary(match.payload).places?.[0]?.connection,
  ).toMatchObject({
    steamId: enrolled.steamId,
    generation: enrolled.generation,
    proof: enrolled.proof,
  })
  endServiceCall({
    call: BigInt(sent.length),
    payload: new Uint8Array(),
    failure: 'none',
    error: '',
  })

  // The read keeps slot zero and the live identity.
  const result: { calls: number; page?: BoardPage } = { calls: 0 }
  readBoard('cup', { player }, (page) => {
    result.calls++
    result.page = page
  })
  const read = sent.at(-1)!
  expect(read.method).toBe('Read')
  expect(BoardQuery.fromBinary(read.payload)).toMatchObject({
    player: 0,
    connection: { generation: 5, steamId: player.steamId },
  })
  expect(result.calls).toBe(0)

  // A later frame returns the private row.
  const page = PageMessage.toBinary({
    player: { rank: 38, steamId: player.steamId },
  })
  endServiceCall({
    call: BigInt(sent.length),
    payload: page,
    failure: 'none',
    error: '',
  })
  expect(result.calls).toBe(1)
  expect(result.page).toEqual({
    rows: [],
    player: {
      rank: 38,
      steamId: player.steamId,
      name: '',
      value: 0,
      tiebreak: 0,
      count: 0,
    },
  })
})
