import { sizeOf } from '@aptre/protobuf-es-lite/binary-encoding'
import type { MessageType } from '@aptre/protobuf-es-lite/message'
import * as scoresPb from '../../src/wasm/scores/scores.pb.js'
import { callService, type Connection } from './host.gen.js'
import { callServiceLater } from './service-later.js'
import { after } from './timer.js'

/** service names the native score service. */
const service = 'scores'

/** requestLimit is the native session's maximum score request size. */
const requestLimit = 16 * 1024

/** tooLarge is the error both the SDK and host give for excess input. */
const tooLarge = 'The score call must fit in 16 KiB'

/** windows encodes the public board window names. */
const windows: Record<BoardWindow, scoresPb.Window> = {
  allTime: scoresPb.Window.ALL_TIME,
  day: scoresPb.Window.DAY,
  week: scoresPb.Window.WEEK,
}

/** BoardWindow is a range of standings in UTC. A week starts Monday. */
export type BoardWindow = 'allTime' | 'day' | 'week'

/** ScoreOptions describes where a score ranks and the run it came from. */
export interface ScoreOptions {
  /** partition names a value for each of the board's partitions. */
  partition?: Record<string, string>
  /** tiebreak ranks equal values, lower first, such as fewer deaths. */
  tiebreak?: number
  /** key identifies the round or run in this game across retries and reloads. */
  key?: string
  /** earnedAt is when the score was earned in Unix milliseconds, or the host's receipt time. */
  earnedAt?: number
  /** evidence names what shows the run, such as a recording. */
  evidence?: string
  /**
   * teammates are the scoring player's teammates on a board that ranks
   * teams, each enrolled with bindScorePlayer: the board's teamSize less one.
   */
  teammates?: readonly ScorePlayer[]
}

/** BoardRead selects a page of a board. Omit window to read all time. */
export interface BoardRead {
  partition?: Record<string, string>
  window?: BoardWindow
  offset?: number
  limit?: number
  player?: Connection
  /**
   * teammates name the player's teammates on a board that ranks teams, so
   * the page's player row is their team's; each is in the game now.
   */
  teammates?: readonly Connection[]
}

/** BoardMember is one player of a team's row. */
export interface BoardMember {
  steamId: bigint
  name: string
}

/**
 * BoardRow is one player's place on a board, or a team's on a board that
 * ranks teams: its members then, ordered by SteamID64, with steamId and name
 * the first member's.
 */
export interface BoardRow {
  rank: number
  steamId: bigint
  name: string
  value: number
  tiebreak: number
  count: number
  members: BoardMember[]
}

/** BoardPage is a page of standings and the requested player's own row. */
export interface BoardPage {
  rows: BoardRow[]
  player?: BoardRow
}

/** ScoreDone hears backend acceptance, or the error and a retry of the same write. */
export type ScoreDone = (
  error?: string,
  retry?: (done: ScoreDone) => void,
) => void

/** ScorePlayer is the connection a mode enrolled with bindScorePlayer. */
export interface ScorePlayer {
  readonly slot: number
  readonly steamId: bigint
  readonly generation: number
  readonly proof: string
}

/** MatchPlace is one enrolled participant's place and optional value. */
export interface MatchPlace {
  player: ScorePlayer
  place: number
  value?: number
}

/**
 * bindScorePlayer captures the connection while the mode serves it. Keep
 * the result with the participant until the score or match is reported.
 */
export function bindScorePlayer(player: Connection): ScorePlayer | undefined {
  // Ask the host to verify the identity before it can be kept for scoring.
  const reply = callService(
    service,
    'Bind',
    scoresPb.ScoreConnection.toBinary(identity(player)),
  )
  const bound =
    reply === undefined ? undefined : scoresPb.ScoreConnection.fromBinary(reply)
  if (!bound?.proof) return undefined
  return {
    slot: bound.slot ?? 0,
    steamId: bound.steamId ?? 0n,
    generation: bound.generation ?? 0,
    proof: bound.proof,
  }
}

/**
 * reportScore submits an enrolled player's result and answers on a later frame.
 * Supply a stable key for each round or run if the game records more than one
 * result per player and board.
 */
export function reportScore(
  player: ScorePlayer,
  board: string,
  value: number,
  done: ScoreDone,
  options: ScoreOptions = {},
): void {
  // Refuse excess text before sizing or encoding the write. The host checks
  // the team against the board.
  const teammates = options.teammates ?? []
  if (
    player.proof.length > 64 ||
    teammates.some((teammate) => teammate.proof.length > 64) ||
    !fitsText(board, options)
  ) {
    after(0, () => done(tooLarge))
    return
  }

  // Use each enrolled connection even if its original slot has been reused.
  const payload = encodeRequest(scoresPb.ScoreReport, {
    ...writeFields(options),
    board,
    player: player.slot,
    value: integer(value),
    tiebreak: integer(options.tiebreak ?? 0),
    connection: player,
    teammates: teammates.map((teammate) => ({
      player: teammate.slot,
      connection: teammate,
    })),
  })
  if (!payload) {
    after(0, () => done(tooLarge))
    return
  }
  submit('Report', payload, done)
}

/** reportMatch submits the enrolled cohort as one result, with shared places for ties. */
export function reportMatch(
  board: string,
  places: readonly MatchPlace[],
  done: ScoreDone,
  options: Omit<ScoreOptions, 'tiebreak' | 'teammates'> = {},
): void {
  // Check cardinality before constructing or encoding the cohort.
  if (places.length === 0 || places.length > 64) {
    after(0, () => done('The match needs one to 64 places'))
    return
  }

  // Do not traverse oversized strings while sizing the placements.
  if (
    places.some(({ player }) => player.proof.length > 64) ||
    !fitsText(board, options)
  ) {
    after(0, () => done(tooLarge))
    return
  }

  // Carry the enrollment with each place so it cannot drift to another player.
  const payload = encodeRequest(scoresPb.MatchReport, {
    ...writeFields(options),
    board,
    places: places.map(({ player, place, value }) => ({
      player: player.slot,
      place,
      value: integer(value ?? 0),
      connection: player,
    })),
  })
  if (!payload) {
    after(0, () => done(tooLarge))
    return
  }
  submit('Match', payload, done)
}

/** readBoard asks for a fresh page and delivers it or an error on a later frame. */
export function readBoard(
  board: string,
  read: BoardRead,
  done: (page: BoardPage | undefined, error?: string) => void,
): void {
  // Keep the size pass bounded even for an oversized board or partition.
  if (!fitsText(board, read)) {
    after(0, () => done(undefined, tooLarge))
    return
  }

  // A viewer row belongs to the current connection named in this read.
  const payload = encodeRequest(scoresPb.BoardQuery, {
    board,
    partition: read.partition,
    window: read.window === undefined ? undefined : windows[read.window],
    offset: read.offset,
    limit: read.limit,
    player: read.player?.player.slot,
    connection: read.player && identity(read.player),
    teammates: read.teammates?.map((teammate) => ({
      player: teammate.player.slot,
      connection: identity(teammate),
    })),
  })
  if (!payload) {
    after(0, () => done(undefined, tooLarge))
    return
  }
  callServiceLater(service, 'Read', payload, (reply, error) => {
    if (reply === undefined) {
      done(undefined, error)
      return
    }
    const page = scoresPb.BoardPage.fromBinary(reply)
    done({
      rows: (page.rows ?? []).map(row),
      player: page.player && row(page.player),
    })
  })
}

/** fitsText bounds the total work needed to size the input as UTF-8. */
function fitsText(
  board: string,
  options: Pick<ScoreOptions, 'partition' | 'key' | 'evidence'>,
): boolean {
  // Each UTF-16 code unit needs at least one byte in protobuf's UTF-8 strings.
  let units =
    board.length + (options.key?.length ?? 0) + (options.evidence?.length ?? 0)
  if (units > requestLimit) {
    return false
  }

  // Even an empty partition pair takes space on the wire.
  for (const name in options.partition) {
    units += 1 + name.length + (options.partition[name]?.length ?? 0)
    if (units > requestLimit) {
      return false
    }
  }
  return true
}

/** encodeRequest sizes the generated request before allocating its binary. */
function encodeRequest<T extends object>(
  type: MessageType<T>,
  request: T,
): Uint8Array | undefined {
  return sizeOf(type, request, requestLimit) <= requestLimit
    ? type.toBinary(request)
    : undefined
}

/** writeFields copies the run identity and board selection into the request. */
function writeFields(options: ScoreOptions) {
  return {
    key: options.key,
    earnedAtUnixMillis: integer(options.earnedAt ?? 0),
    partition: options.partition,
    evidence: options.evidence,
  }
}

/** submit sends the immutable payload, including its key and time, on each retry. */
function submit(
  method: 'Report' | 'Match',
  payload: Uint8Array,
  done: ScoreDone,
): void {
  callServiceLater(service, method, payload, (reply, error) => {
    if (reply !== undefined) {
      done()
      return
    }
    done(error ?? 'The host did not confirm the score', (next) =>
      submit(method, payload, next),
    )
  })
}

/** identity encodes a connection without looking up the slot's present occupant. */
function identity(connection: Connection): scoresPb.ScoreConnection {
  return {
    slot: connection.player.slot,
    steamId: connection.steamId,
    generation: connection.generation,
  }
}

/** integer stores the score in the board's integer unit. */
function integer(value: number): bigint {
  return BigInt(Math.round(value))
}

/** row converts a wire standing to the public SDK's number fields. */
function row(standing: scoresPb.Standing): BoardRow {
  return {
    rank: standing.rank ?? 0,
    steamId: standing.steamId ?? 0n,
    name: standing.name ?? '',
    value: Number(standing.value ?? 0n),
    tiebreak: Number(standing.tiebreak ?? 0n),
    count: standing.count ?? 0,
    members: (standing.members ?? []).map((member) => ({
      steamId: member.steamId ?? 0n,
      name: member.name ?? '',
    })),
  }
}
