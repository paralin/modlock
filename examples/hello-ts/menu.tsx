// The menu is a panel each player opens with /menu. It shows the player's
// hero and offers buttons that call back into the mod.
import { hide, type Player, players, show, toast } from 'modlock'

import { greet, nameOf, placeSign } from './hello'

/** open holds the slot of each player whose menu is open. */
const open = new Set<number>()

/** openMenu shows player the menu until they close it or leave. */
export function openMenu(player: Player): void {
  open.add(player.slot)
  draw(player)
}

/** closeMenu removes the menu from player's screen. */
export function closeMenu(player: Player): void {
  open.delete(player.slot)
  hide(player)
}

/**
 * drawMenus redraws every open menu with its player's current hero. show
 * sends only what changed, so redrawing each frame costs little.
 */
export function drawMenus(): void {
  const present = new Set(players().map((c) => c.player.slot))
  for (const slot of open) {
    if (!present.has(slot)) {
      open.delete(slot)
    }
  }
  for (const connection of players()) {
    if (open.has(connection.player.slot)) {
      draw(connection.player)
    }
  }
}

/** draw shows player their menu as it stands now. */
function draw(player: Player): void {
  const pawn = player.pawn()
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
      <label style={{ fontSize: 28, bold: true, color: '#ffd34d' }}>
        Hello, {nameOf(player)}
      </label>
      {pawn === undefined ? (
        <label>No hero yet</label>
      ) : (
        <panel style={{ flow: 'down', margin: [8, 0] }}>
          <label>
            Health {Math.max(0, pawn.health)} / {pawn.maxHealth}
          </label>
          <label>Souls {pawn.souls}</label>
        </panel>
      )}
      <MenuButton label="Greet me" onPress={(p) => greet(p, nameOf(p))} />
      <MenuButton
        label="Raise a sign"
        onPress={(p) => {
          if (!placeSign(p, nameOf(p))) {
            toast(p, 'You need a living hero to hold a sign.')
          }
        }}
      />
      <MenuButton label="Wave to everyone" onPress={wave} />
      <MenuButton label="Close" onPress={closeMenu} />
    </panel>,
  )
}

/** MenuButton is one row of the menu. */
function MenuButton(props: {
  label: string
  onPress: (player: Player) => void
}) {
  return (
    <button
      style={{
        width: 220,
        margin: [4, 0],
        padding: [6, 12],
        background: '#2a3a4a',
        borderRadius: 4,
      }}
      onPress={props.onPress}
    >
      <label>{props.label}</label>
    </button>
  )
}

/** wave tells every player that player waved. */
function wave(player: Player): void {
  const name = nameOf(player)
  for (const connection of players()) {
    connection.player.announce('Hello!', `${name} waves to everyone.`)
  }
}
