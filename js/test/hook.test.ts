import { describe, expect, it } from 'bun:test'

import {
  Hook,
  type HookAbility,
  type HookPlayer,
  hookAbility,
} from '../src/hook.js'

/** FakeHero is a hero with the hook, recording the writes made to it. */
class FakeHero implements HookPlayer {
  writes: string[] = []

  constructor(public hook: HookAbility | undefined) {}

  abilities(): readonly HookAbility[] {
    return this.hook ? [this.hook] : []
  }

  setAbility(options: { upgrades?: number; cooldownEnd?: number }): boolean {
    this.writes.push(`set ${JSON.stringify(options)}`)
    return true
  }

  refreshAbility(ability: string): boolean {
    this.writes.push(`refresh ${ability}`)
    return true
  }
}

/** hookState is the hook as the game shows it. */
function hookState(state: Partial<HookAbility> = {}): HookAbility {
  return {
    name: hookAbility,
    entity: 55,
    upgrades: 0b1111,
    cooldownStart: 0,
    cooldownEnd: 0,
    ...state,
  }
}

describe('Hook', () => {
  it('reads the victim from the hook entity', () => {
    // Read a hook that holds hero 77 and check what the host was asked.
    const asked: unknown[][] = []
    const hook = new Hook((...args) => {
      asked.push(args)
      return 77
    })
    const read = hook.update(new FakeHero(hookState()), 1)
    expect(read).toEqual({ victim: 77, thrown: false })
    expect(asked).toEqual([
      [55, 'CCitadel_Ability_Hook', 'm_hHookVictim', 'handle'],
    ])
  })

  it('reads no victim from a field that is not a handle', () => {
    const hook = new Hook(() => undefined)
    expect(hook.update(new FakeHero(hookState()), 1)?.victim).toBeUndefined()
  })

  it('reports nothing for a hero without the hook', () => {
    const hook = new Hook(() => 0)
    expect(hook.update(new FakeHero(undefined), 1)).toBeUndefined()
  })

  it('unlocks the hook with every upgrade', () => {
    // Update a hook with one upgrade and a fully upgraded one.
    const hook = new Hook(() => 0)
    const low = new FakeHero(hookState({ upgrades: 0b11 }))
    hook.update(low, 1)
    const full = new FakeHero(hookState())
    hook.update(full, 1)
    expect(low.writes).toEqual([
      'set {"ability":"citadel_ability_hook","upgrades":15}',
    ])
    expect(full.writes).toEqual([])
  })

  it('counts each cooldown once and ends it', () => {
    // Throw at 2 s, watch two frames of the cooldown, then throw again.
    const hook = new Hook(() => 0)
    const hero = new FakeHero(hookState({ cooldownStart: 2, cooldownEnd: 5 }))
    const first = hook.update(hero, 2.1)
    const again = hook.update(hero, 2.2)
    hero.hook = hookState({ cooldownStart: 2.3, cooldownEnd: 5.3 })
    const second = hook.update(hero, 2.4)
    expect([first?.thrown, again?.thrown, second?.thrown]).toEqual([
      true,
      false,
      true,
    ])
    expect(hero.writes).toEqual([
      `refresh ${hookAbility}`,
      `refresh ${hookAbility}`,
      `refresh ${hookAbility}`,
    ])
  })

  it('counts no throw once the cooldown has ended', () => {
    const hook = new Hook(() => 0)
    const hero = new FakeHero(hookState({ cooldownStart: 2, cooldownEnd: 5 }))
    expect(hook.update(hero, 5)?.thrown).toBe(false)
    expect(hero.writes).toEqual([])
  })

  it('counts a throw again after a reset', () => {
    // Count a throw, forget it, and see the same cooldown count again.
    const hook = new Hook(() => 0)
    const hero = new FakeHero(hookState({ cooldownStart: 2, cooldownEnd: 5 }))
    hook.update(hero, 2.1)
    hook.reset()
    expect(hook.update(hero, 2.2)?.thrown).toBe(true)
  })

  it('sets the cooldown to its length from the throw', () => {
    // A three second hook that the game set to five seconds is cut to three.
    const hook = new Hook(() => 0, 3)
    const hero = new FakeHero(hookState({ cooldownStart: 10, cooldownEnd: 15 }))
    expect(hook.update(hero, 10.5)?.thrown).toBe(true)
    expect(hero.writes).toEqual([
      'set {"ability":"citadel_ability_hook","cooldownEnd":13}',
    ])

    // A cooldown already that long is left alone.
    const kept = new FakeHero(hookState({ cooldownStart: 10, cooldownEnd: 13 }))
    hook.update(kept, 10.6)
    expect(kept.writes).toEqual([])
  })
})
