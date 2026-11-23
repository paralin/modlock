// The panel lists one player's controllers as the server sees them.
import {
  type Controller,
  type ControllerStream,
  type Player,
  type PlayerControllers,
  show,
} from 'modlock'

/** streamText says what each stream state means to the player. */
const streamText: Record<ControllerStream, string> = {
  none: 'Waiting for your Modlock client',
  live: 'Live',
  unfocused: 'Deadlock is not focused',
  stale: 'No update for a moment',
}

/** streamColor colors the stream line by state. */
const streamColor: Record<ControllerStream, string> = {
  none: '#a0a8b0',
  live: '#7ee081',
  unfocused: '#ffd34d',
  stale: '#ff8a5c',
}

/**
 * drawPanel shows player their controllers. show sends only the parts that
 * changed since the last draw, so drawing on every frame costs little.
 */
export function drawPanel(
  player: Player,
  entry: PlayerControllers | undefined,
): void {
  const stream = entry?.stream ?? 'none'
  show(
    player,
    <panel
      style={{
        flow: 'down',
        horizontalAlign: 'right',
        verticalAlign: 'center',
        margin: [0, 40, 0, 0],
        padding: 16,
        background: '#101820e0',
        borderRadius: 8,
      }}
    >
      <label style={{ fontSize: 24, bold: true, color: '#ffd34d' }}>
        Controllers
      </label>
      <label style={{ color: streamColor[stream] }}>{streamText[stream]}</label>
      {(entry?.controllers ?? []).map((c) => device(c))}
    </panel>,
  )
}

/** device lists one controller's inputs. */
function device(c: Controller) {
  const { info, state } = c
  const held = state.buttons.flatMap((down, i) => (down ? [i] : []))
  return (
    <panel style={{ flow: 'down', margin: [8, 0, 0, 0] }}>
      <label style={{ bold: true }}>{info.name || `Device ${info.id}`}</label>
      <label>Axes {state.axes.map((v) => v.toFixed(2)).join('  ')}</label>
      <label>Buttons {held.length === 0 ? 'none' : held.join(' ')}</label>
      {state.hats.length > 0 ? (
        <label>Hats {state.hats.join(' ')}</label>
      ) : null}
      {info.gamepad ? (
        <label>
          Sticks {fixed(state.gamepad.leftX)}, {fixed(state.gamepad.leftY)} /{' '}
          {fixed(state.gamepad.rightX)}, {fixed(state.gamepad.rightY)} Triggers{' '}
          {fixed(state.gamepad.leftTrigger)} {fixed(state.gamepad.rightTrigger)}
          {state.gamepad.held.length > 0
            ? `  ${state.gamepad.held.join(' ')}`
            : ''}
        </label>
      ) : null}
    </panel>
  )
}

/** fixed prints v to two places. */
function fixed(v: number): string {
  return v.toFixed(2)
}
