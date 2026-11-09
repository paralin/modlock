/** StockGuardState is the part of the guard's state a reader judges. */
type StockGuardState = {
  game: string
  hidden: boolean
  fresh: boolean
  left: boolean
}

export const title = "The game's HUD under Modlock's stock guard"

/** allowed reports that the screen may hold the game's HUD. */
const allowed = (value: StockGuardState) =>
  value.fresh && !value.left && value.game !== 'hideout'

/** place names where the player is. */
const place = (value: StockGuardState) =>
  ({
    mod: "On the mod's server",
    hideout: 'In the hideout',
    other: 'In another game',
  })[value.game] ?? 'In another game'

/** state says where the player is and whose HUD they see. */
export const state = (value: StockGuardState) => {
  if (!value.hidden) return `${place(value)}: the game's own HUD`
  return allowed(value)
    ? `${place(value)}: the mod hides parts`
    : `${place(value)}: parts hidden until the next tick`
}

/** step names each arrow as the player or the guard sees it happen. */
export const step = (
  action: string,
  from: StockGuardState,
  to: StockGuardState,
) => {
  if (state(from) === state(to)) return null
  return (
    (
      {
        Join: "You join the mod's server",
        Screen: 'A screen arrives',
        LateScreen: 'A screen sent before you left arrives',
        Leave: 'You leave, and the game says so',
        LeaveUnseen: 'The server goes away unannounced',
        Expire: 'Thirty seconds pass with no title',
        Reload: 'The renderer reloads',
        Tick: "The guard's tick",
        StopSession: 'The session stops',
      } as Record<string, string>
    )[action] ?? null
  )
}
