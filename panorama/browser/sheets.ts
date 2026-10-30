// sheets reads a Panorama style sheet, as the preview serves a built one, into
// rules whose selectors match the DOM stand-in's elements, and finds the
// declarations that win on each element, so the stand-in can write them
// through the same mapping inline styles use.

/** Rule is one selector of a sheet's rule, with its declarations. */
export interface Rule {
  /** selector matches the stand-in's elements, as querySelectorAll takes it. */
  readonly selector: string
  /** specificity ranks the rule: ids, then classes, then panel types. */
  readonly specificity: number
  /** declarations are the rule's properties, in kebab case, and values. */
  readonly declarations: readonly (readonly [string, string])[]
}

/**
 * parseSheet reads a sheet's rules in order. It substitutes each @define,
 * skips @keyframes and other at-rule blocks, which the stand-in does not run,
 * and drops !important, which Panorama ignores.
 */
export function parseSheet(source: string): Rule[] {
  // Take the defines out first, so a rule may use one defined below it.
  const defines = new Map<string, string>()
  const text = source
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(
      /@define\s+([\w-]+)\s*:\s*([^;]*);/g,
      (_, name: string, value: string) => {
        defines.set(name, value.trim())
        return ''
      },
    )
  const define = (value: string) =>
    value.replace(/[\w-]+/g, (word) => defines.get(word) ?? word)

  // Walk the top-level blocks, skipping at-rules with their nested blocks.
  const rules: Rule[] = []
  let start = 0
  for (let i = 0; i < text.length; i++) {
    if (text[i] !== '{') continue
    const prelude = text.slice(start, i).trim()
    let depth = 1
    let end = i + 1
    for (; end < text.length && depth > 0; end++) {
      if (text[end] === '{') depth++
      else if (text[end] === '}') depth--
    }
    const body = text.slice(i + 1, end - 1)
    start = i = end
    if (prelude.startsWith('@')) continue

    // Read the declarations, then give each selector of the list a rule.
    const declarations = body.split(';').flatMap((declaration) => {
      const colon = declaration.indexOf(':')
      const name = declaration.slice(0, colon).trim().toLowerCase()
      const value = define(declaration.slice(colon + 1))
        .replace(/!important/i, '')
        .trim()
      return colon > 0 && name && value ? [[name, value] as const] : []
    })
    for (const selector of prelude.split(',')) {
      const trimmed = selector.trim()
      if (trimmed)
        rules.push({
          selector: translate(trimmed),
          specificity: specificity(trimmed),
          declarations,
        })
    }
  }
  return rules
}

/**
 * translate writes a Panorama selector in the stand-in's terms: a panel type
 * becomes its p- class, an id its data-id attribute, and :disabled and
 * :selected the class and attribute the stand-in keeps them in. A pseudo-class
 * the stand-in has no state for stays, so the rule fails to match and drops.
 */
export function translate(selector: string): string {
  return selector
    .replace(/(^|[\s>+~(])([A-Z]\w*)/g, '$1.p-$2')
    .replace(/#([\w-]+)/g, '[data-id="$1"]')
    .replace(/:disabled\b/g, '.disabled')
    .replace(/:selected\b/g, '[aria-pressed="true"]')
}

/** specificity counts a selector's ids, classes and pseudo-classes, and types. */
export function specificity(selector: string): number {
  const ids = selector.match(/#[\w-]+/g)?.length ?? 0
  const classes = selector.match(/[.:][\w-]+/g)?.length ?? 0
  const types = selector.match(/(^|[\s>+~])[A-Za-z]\w*/g)?.length ?? 0
  return ids * 10000 + classes * 100 + types
}

/** Declared is a property's winning value so far and its rule's rank. */
interface Declared {
  readonly value: string
  readonly rank: number
}

/** Scope is a loaded sheet's rules and the element whose subtree they style. */
export interface Scope {
  readonly element: Element
  readonly rules: readonly Rule[]
}

/**
 * cascade returns the winning declarations on each element the scopes'
 * rules match, later scopes and rules winning ties. A selector the browser
 * cannot parse matches nothing.
 */
export function cascade(
  scopes: Iterable<Scope>,
): Map<Element, Map<string, string>> {
  const winners = new Map<Element, Map<string, Declared>>()
  let order = 0
  for (const { element, rules } of scopes) {
    for (const rule of rules) {
      // Rank by specificity, then by position across every scope.
      const rank = rule.specificity * 1e6 + order++
      let matched: Element[]
      try {
        matched = [...element.querySelectorAll(rule.selector)]
        if (element.matches(rule.selector)) matched.push(element)
      } catch {
        continue
      }
      for (const each of matched) {
        let declared = winners.get(each)
        if (!declared)
          winners.set(each, (declared = new Map<string, Declared>()))
        for (const [name, value] of rule.declarations) {
          if ((declared.get(name)?.rank ?? -1) <= rank)
            declared.set(name, { value, rank })
        }
      }
    }
  }
  return new Map(
    [...winners].map(([element, declared]) => [
      element,
      new Map([...declared].map(([name, { value }]) => [name, value])),
    ]),
  )
}
