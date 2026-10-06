// The bar at the top of every player's screen: each team's hold on the hill.
import { players, show } from 'modlock'

import { type Hill, type Team, winScore } from './hill'

/** drawBars shows every player the teams' progress toward winning. */
export function drawBars(hill: Hill): void {
  for (const connection of players()) {
    show(connection.player, <Bars hill={hill} />)
  }
}

/** Bars is the whole bar: a status line over one row per team. */
function Bars(props: { hill: Hill }) {
  const { hill } = props
  return (
    <panel
      style={{
        flow: 'down',
        horizontalAlign: 'center',
        verticalAlign: 'top',
        margin: [60, 0, 0, 0],
        padding: 10,
        width: 360,
        background: '#101820d0',
        borderRadius: 6,
      }}
    >
      <label style={{ bold: true, textAlign: 'center', width: 'fill' }}>
        {status(hill)}
      </label>
      {hill.teams.map((team) => (
        <Progress team={team} />
      ))}
    </panel>
  )
}

/** Progress is one team's name and a bar that fills toward winScore. */
function Progress(props: { team: Team }) {
  const { team } = props
  const percent = Math.min(100, Math.round((team.score / winScore) * 100))
  return (
    <panel style={{ flow: 'right', width: 'fill', margin: [6, 0, 0, 0] }}>
      <label style={{ width: 90, color: team.color }}>{team.name}</label>
      <panel style={{ width: 'fill', height: 14, background: '#ffffff20' }}>
        <panel
          style={{
            width: `${percent}%`,
            height: 'fill',
            background: team.color,
          }}
        />
      </panel>
    </panel>
  )
}

/** status says what is happening on the hill. */
function status(hill: Hill): string {
  // Outside a round, say what comes next.
  if (hill.center === undefined) {
    return 'Type /hill to raise the hill where you stand'
  }
  if (hill.winner) {
    return `${hill.winner.name} wins! Next round soon`
  }

  // During a round, say who holds the hill.
  if (hill.hold === 'nobody') {
    return 'Nobody holds the hill'
  }
  if (hill.hold === 'contested') {
    return 'The hill is contested'
  }
  return `${hill.hold.name} holds the hill`
}
