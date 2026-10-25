// Hook is Bebop's hook for a mod that plays on it: the catch read, throw
// detection, the cooldown length and the upgrade lock. A mod keeps one Hook per
// thrower, calls update each frame the thrower's hero lives, and scores what it
// reports. It touches no host call but the field read a mod passes in, such as
// readField, so a test can run it without the game.

/** hookAbility is the designer name of Bebop's hook. */
export const hookAbility = 'citadel_ability_hook'

/** hookUpgrades unlocks the hook with all three upgrades. */
export const hookUpgrades = 0b1111

/** HookAbility is the part of the host's Ability that Hook reads. */
export interface HookAbility {
  readonly name: string
  readonly entity: number
  readonly upgrades: number
  readonly cooldownStart: number
  readonly cooldownEnd: number
}

/** HookPlayer is the part of the host's Player that Hook moves. */
export interface HookPlayer {
  abilities(): readonly HookAbility[]
  setAbility(options: {
    ability: string
    upgrades?: number
    cooldownEnd?: number
  }): boolean
  refreshAbility(ability: string): boolean
}

/** ReadHandle is the host's readField, which Hook calls for the hook's victim. */
export type ReadHandle = (
  entity: number,
  className: string,
  field: string,
  type: 'handle',
) => unknown

/**
 * HookRead is what the hook did this frame: the hero it holds, if any, and
 * whether a new throw began since the last frame Hook reported one.
 */
export interface HookRead {
  readonly victim: number | undefined
  readonly thrown: boolean
}

/**
 * Hook watches one thrower's hook. Its cooldown lasts cooldownSeconds from
 * each throw; zero ends it at once, so the hook is always ready.
 */
export class Hook {
  /** thrownAt is the start of the latest cooldown counted as a throw. */
  private thrownAt = 0

  constructor(
    private readonly read: ReadHandle,
    private readonly cooldownSeconds = 0,
  ) {}

  /** reset forgets the throws counted so far, for a thrower in a new world. */
  reset(): void {
    this.thrownAt = 0
  }

  /**
   * update reads the thrower's hook at game time now, keeps it fully upgraded
   * and sets the length of any cooldown a throw started. It reports nothing
   * for a hero without the hook.
   */
  update(player: HookPlayer, now: number): HookRead | undefined {
    // Find the hook and read the hero it holds.
    const ability = player.abilities().find((a) => a.name === hookAbility)
    if (!ability) {
      return undefined
    }
    const victim = this.victim(ability.entity)

    // Keep the hook unlocked with every upgrade.
    if ((ability.upgrades & hookUpgrades) !== hookUpgrades) {
      player.setAbility({ ability: hookAbility, upgrades: hookUpgrades })
    }

    // A cooldown that started since the last one counted is a throw.
    if (ability.cooldownEnd <= now) {
      return { victim, thrown: false }
    }
    const thrown = ability.cooldownStart > this.thrownAt
    if (thrown) {
      this.thrownAt = ability.cooldownStart
    }

    // Set the cooldown's length: none ends it, else it ends that long after the throw.
    this.setCooldown(player, ability)
    return { victim, thrown }
  }

  /** victim is the hero the hook entity holds, or undefined while it holds none. */
  private victim(hookEntity: number): number | undefined {
    const victim = this.read(
      hookEntity,
      'CCitadel_Ability_Hook',
      'm_hHookVictim',
      'handle',
    )
    return typeof victim === 'number' ? victim : undefined
  }

  /** setCooldown ends the cooldown cooldownSeconds after the hook's throw. */
  private setCooldown(player: HookPlayer, ability: HookAbility): void {
    if (this.cooldownSeconds <= 0) {
      player.refreshAbility(hookAbility)
      return
    }
    const cooldownEnd = ability.cooldownStart + this.cooldownSeconds
    if (cooldownEnd !== ability.cooldownEnd) {
      player.setAbility({ ability: hookAbility, cooldownEnd })
    }
  }
}
