// Who the player hears on voice chat, read from the game's own interface.
//
// The game keeps no voice event a script can hear. Its client marks each
// player's panel in the stock top bar with VoiceActive (or PartyVoiceActive
// for party chat) while it plays that player's voice, and the marks stay
// while a mod hides the top bar, so the reader takes them there. A player is
// known by the name the top bar shows, the same name the server holds for
// their connection.
//
// A name stays heard for holdSeconds after its mark goes off, so a pause
// between words does not blink what shows it.

/** VOICE_MARKS are the classes the top bar keeps on a player it plays. */
const VOICE_MARKS = ['VoiceActive', 'PartyVoiceActive'] as const

/** TOP_BAR is the id of the game's top bar in its HUD. */
const TOP_BAR = 'TopBar'

/** VoicePanel is the part of a game panel the reader touches. */
export interface VoicePanel {
  readonly text?: string
  IsValid(): boolean
  BHasClass?(name: string): boolean
  GetParent?(): VoicePanel | null
  FindChildTraverse(id: string): VoicePanel | null
  FindChildrenWithClassTraverse?(name: string): VoicePanel[]
}

/** holdSeconds is how long a name stays heard after its mark goes off. */
export const holdSeconds = 0.8

/** rescanReads is the reads between walks for the top bar's player panels. */
const rescanReads = 10

/**
 * Voices reads who the player hears from the top bar in the interface root
 * sits in. Each read returns the names heard now or within holdSeconds.
 */
export class Voices {
  /** topBar is the game's top bar, once found. */
  private topBar: VoicePanel | null = null

  /** players are the top bar's player panels, from the last walk. */
  private players: VoicePanel[] = []

  /** reads counts reads, to walk the top bar again now and then. */
  private reads = 0

  /** last holds when each heard name's mark was last on, in seconds. */
  private readonly last = new Map<string, number>()

  constructor(private readonly root: VoicePanel) {}

  /**
   * read takes the top bar's marks at time now, in seconds, and returns the
   * names heard. It returns none while no top bar is found.
   */
  read(now: number): ReadonlySet<string> {
    // Find the top bar, and walk it for player panels now and then, since
    // players join and the game rebuilds its HUD.
    if (!this.topBar?.IsValid()) {
      this.topBar = findTopBar(this.root)
      this.players = []
    }
    const walk = this.reads++ % rescanReads === 0 || !this.players.every(valid)
    if (this.topBar && walk) this.players = topBarPlayers(this.topBar)

    // Note the names marked now, then keep those within the hold.
    for (const player of this.players) {
      if (!player.IsValid() || !speaking(player)) continue
      const name = playerName(player)
      if (name) this.last.set(name, now)
    }
    const heard = new Set<string>()
    for (const [name, at] of this.last) {
      if (now - at <= holdSeconds) heard.add(name)
      else this.last.delete(name)
    }
    return heard
  }
}

/** findTopBar returns the game's top bar in the interface panel sits in. */
function findTopBar(panel: VoicePanel): VoicePanel | null {
  let top = panel
  for (let up = top.GetParent?.(); up; up = up.GetParent?.()) top = up
  return top.FindChildTraverse(TOP_BAR)
}

/**
 * topBarPlayers returns the top bar's player panels: the parent of each voice
 * bar the player layout carries.
 */
export function topBarPlayers(topBar: VoicePanel): VoicePanel[] {
  const players: VoicePanel[] = []
  for (const bar of topBar.FindChildrenWithClassTraverse?.('VoiceBar') ?? []) {
    const player = bar.GetParent?.()
    if (player) players.push(player)
  }
  return players
}

/** speaking reports whether the client is playing a top bar player's voice. */
function speaking(player: VoicePanel): boolean {
  return VOICE_MARKS.some((mark) => player.BHasClass?.(mark))
}

/** playerName reads the name a top bar player panel shows. */
function playerName(player: VoicePanel): string {
  return (
    player.FindChildrenWithClassTraverse?.('PlayerName')[0]?.text?.trim() ?? ''
  )
}

/** valid reports whether a panel still exists. */
function valid(panel: VoicePanel): boolean {
  return panel.IsValid()
}
