// Scores report a player's results to the boards a mod declares in mod.json
// and read those boards back, over the host's scores service. A host without
// the service, such as a server with no backend, takes no score; the mod
// logs each one instead, so its author sees what it would report.
//
//   reportScore(player, 'trial.time', elapsedMs, { partition: { course: 'canyon' }, key: runId })

import * as scoresPb from '../../src/wasm/scores/scores.pb.js'
import { callService, log, type Player } from './host.gen.js'

/** service names the host service that keeps scores. */
const service = 'scores'

/** BoardWindow is a span of time a board keeps standings for, in UTC; a week starts Monday. */
export type BoardWindow = 'allTime' | 'day' | 'week'

/** windowValues encodes each BoardWindow. */
const windowValues: Record<BoardWindow, scoresPb.Window> = {
  allTime: scoresPb.Window.ALL_TIME,
  day: scoresPb.Window.DAY,
  week: scoresPb.Window.WEEK,
}

/** ScoreOptions describe where a score ranks and the run it came from. */
export interface ScoreOptions {
  /** partition names a value for each of the board's partitions. */
  partition?: Record<string, string>
  /** tiebreak ranks equal values, lower first, such as fewer deaths. */
  tiebreak?: number
  /** key identifies the run, so a resent score counts once. */
  key?: string
  /** earnedAt is when the score was earned, in Unix milliseconds; the host's clock by default. */
  earnedAt?: number
  /** evidence names what shows the run, such as a recording. */
  evidence?: string
}

/** MatchPlace is one player's finishing place in a match, 1 for the winner. */
export interface MatchPlace {
  player: Player
  place: number
  /** value is the player's score in the match, for a board that keeps one. */
  value?: number
}

/** BoardRead selects a page of a board. */
export interface BoardRead {
  /** partition names a value for each of the board's partitions. */
  partition?: Record<string, string>
  /** window is one of the board's windows; all time by default. */
  window?: BoardWindow
  /** offset skips that many rows from the top. */
  offset?: number
  /** limit bounds the rows; ten by default. */
  limit?: number
  /** player asks for this player's own row too. */
  player?: Player
}

/** BoardRow is one player's place on a board. */
export interface BoardRow {
  rank: number
  steamId: bigint
  name: string
  value: number
  tiebreak: number
  count: number
}

/** BoardPage is a page of a board. */
export interface BoardPage {
  /** rows are the standings in rank order. */
  rows: BoardRow[]
  /** player is the asked player's row, when they hold a standing. */
  player?: BoardRow
  /** loading is set until the host has read the board; read again on a later frame. */
  loading: boolean
}

/**
 * reportScore reports one score for player to a board the manifest declares.
 * It reports whether the host took it; a host without the scores service
 * takes none, and the mod logs the score instead.
 */
export function reportScore(
  player: Player,
  board: string,
  value: number,
  options: ScoreOptions = {},
): boolean {
  const report = scoresPb.ScoreReport.toBinary({
    board,
    player: player.slot,
    value: int64(value),
    tiebreak: int64(options.tiebreak ?? 0),
    partition: options.partition,
    key: options.key,
    earnedAtUnixMillis: int64(options.earnedAt ?? 0),
    evidence: options.evidence,
  })
  return taken(
    'Report',
    report,
    `${board}${describe(options.partition)}: player ${player.slot} ${value}`,
  )
}

/**
 * reportMatch reports one match's places to a rating or match board the
 * manifest declares. Players who tie, such as one team's members, share a
 * place. It reports whether the host took it, as reportScore does.
 */
export function reportMatch(
  board: string,
  places: readonly MatchPlace[],
  options: Omit<ScoreOptions, 'tiebreak'> = {},
): boolean {
  const report = scoresPb.MatchReport.toBinary({
    board,
    places: places.map((p) => ({
      player: p.player.slot,
      place: p.place,
      value: int64(p.value ?? 0),
    })),
    partition: options.partition,
    key: options.key,
    earnedAtUnixMillis: int64(options.earnedAt ?? 0),
    evidence: options.evidence,
  })
  const line = places
    .map((p) => `player ${p.player.slot} #${p.place}`)
    .join(', ')
  return taken(
    'Match',
    report,
    `${board}${describe(options.partition)}: ${line}`,
  )
}

/**
 * readBoard reads a page of a board the manifest declares, as the host last
 * read it, or undefined when the host serves no scores. A page the host has
 * not read yet comes back loading.
 */
export function readBoard(
  board: string,
  read: BoardRead = {},
): BoardPage | undefined {
  // Ask the host for the page; a host without scores answers nothing.
  const query = scoresPb.BoardQuery.toBinary({
    board,
    partition: read.partition,
    window: read.window === undefined ? undefined : windowValues[read.window],
    offset: read.offset,
    limit: read.limit,
    player: read.player?.slot,
  })
  const reply = callService(service, 'Read', query)
  if (reply === undefined) return undefined
  const page = scoresPb.BoardPage.fromBinary(reply)
  return {
    rows: (page.rows ?? []).map(row),
    player: page.player ? row(page.player) : undefined,
    loading: page.loading ?? false,
  }
}

/** taken calls a reporting method and logs line when the host takes no score. */
function taken(method: string, report: Uint8Array, line: string): boolean {
  if (callService(service, method, report) !== undefined) return true
  log(`score ${line} (not reported: the host keeps no scores)`)
  return false
}

/** row converts a standing to a BoardRow. */
function row(standing: scoresPb.Standing): BoardRow {
  return {
    rank: standing.rank ?? 0,
    steamId: standing.steamId ?? 0n,
    name: standing.name ?? '',
    value: Number(standing.value ?? 0n),
    tiebreak: Number(standing.tiebreak ?? 0n),
    count: standing.count ?? 0,
  }
}

/** int64 converts a number to the int64 a report carries, rounding a fraction. */
function int64(value: number): bigint {
  return BigInt(Math.round(value))
}

/** describe writes partition values as "[course=canyon]", in name order. */
function describe(partition: Record<string, string> | undefined): string {
  const names = Object.keys(partition ?? {}).sort()
  if (!names.length) return ''
  return `[${names.map((name) => `${name}=${partition![name]}`).join(' ')}]`
}
