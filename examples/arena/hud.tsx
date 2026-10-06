// The scoreboard every player sees: the phase of the match, the fighters'
// scores, and what the player can do next.
import { show } from 'modlock'

import { type Fighter, killTarget, type Match } from './match'

/** gold marks the player's own row and the titles. */
const gold = '#ffd34d'

/** drawScoreboards shows each player the scoreboard as the match stands. */
export function drawScoreboards(match: Match): void {
  // Rank the fighters once for every board.
  const ranked = match
    .entrants()
    .sort((a, b) => b.kills - a.kills || a.deaths - b.deaths)

  // show sends only what changed, so redrawing every frame costs little.
  for (const fighter of match.fighters.values()) {
    show(
      fighter.player,
      <Scoreboard match={match} ranked={ranked} viewer={fighter} />,
    )
  }
}

/** Scoreboard is one player's board. */
function Scoreboard(props: {
  match: Match
  ranked: Fighter[]
  viewer: Fighter
}) {
  // A player who chose the short board sees the top three.
  const { match, viewer } = props
  const short = viewer.player.setting('scoreboard') === 'top'
  const rows = short ? props.ranked.slice(0, 3) : props.ranked

  return (
    <panel
      style={{
        flow: 'down',
        horizontalAlign: 'left',
        verticalAlign: 'top',
        margin: [120, 0, 0, 24],
        padding: 12,
        width: 260,
        background: '#101820d0',
        borderRadius: 6,
      }}
    >
      <label style={{ fontSize: 22, bold: true, color: gold }}>
        {headline(match)}
      </label>
      {rows.map((fighter) => (
        <Row fighter={fighter} own={fighter === viewer} />
      ))}
      {viewer.entered ? null : (
        <label style={{ margin: [8, 0, 0, 0] }}>Type /ready to fight</label>
      )}
    </panel>
  )
}

/** Row is one fighter's name and score. */
function Row(props: { fighter: Fighter; own: boolean }) {
  const { fighter, own } = props
  return (
    <panel style={{ flow: 'right', width: 'fill', margin: [4, 0, 0, 0] }}>
      <label style={{ width: 'fill', color: own ? gold : '#ffffff' }}>
        {fighter.name}
      </label>
      <label style={{ width: 70, textAlign: 'right' }}>
        {fighter.kills} / {fighter.deaths}
      </label>
    </panel>
  )
}

/** headline says where the match stands. */
function headline(match: Match): string {
  const left = Math.max(0, Math.ceil(match.deadline - match.now))
  switch (match.phase) {
    case 'waiting':
      return `Waiting: ${match.entrants().length} of 2 ready`
    case 'countdown':
      return `Starting in ${left}`
    case 'live':
      return `First to ${killTarget} · ${clock(left)}`
    case 'results':
      return match.result
  }
}

/** clock formats seconds as minutes and seconds. */
function clock(seconds: number): string {
  return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`
}
