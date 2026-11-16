import { create, type Tokenizer } from 'zbsearch'

/**
 * identifierRun matches a run of characters the English tokenizer keeps in one
 * token and that can hold an identifier's parts: `replaceAbility`,
 * `citadel_ability_hornet_snipe`, `PRE_UNIT_HEALED`.
 */
const identifierRun = /[A-Za-z0-9_]+/g

/**
 * identifierParts splits an identifier at underscores and at camelCase
 * boundaries, `HTTPServer` into `HTTP` and `Server`, and lowercases the parts.
 */
export function identifierParts(word: string): string[] {
  return word
    .split('_')
    .flatMap((piece) =>
      piece.split(/(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])/),
    )
    .filter((part) => part.length > 0)
    .map((part) => part.toLowerCase())
}

/**
 * searchTokenizer is the English tokenizer, which keeps an identifier such as
 * `replaceAbility` as the one token `replaceability`, plus, for each
 * identifier of more than one part, every part (`replace`, `ability`) and
 * every tail of the parts joined (`ability`). A search for `replace ability`,
 * `ability`, `AbilityCast` or `hornet` then finds the identifier as well, and
 * a prefix of a part or tail, `abilityca`, finds it too. Only the index
 * needs it: the browser tokenizes a query with the stock tokenizer, so
 * `replaceAbility` stays the one token `replaceability`, and a query of one
 * identifier matches that identifier rather than every name with an
 * `ability` in it.
 */
export function searchTokenizer(): Tokenizer {
  const base = create({
    schema: { _: 'string' },
    language: 'english',
  }).tokenizer
  return {
    ...base,
    tokenize(input, language, prop, withCache) {
      const tokens = base.tokenize(input, language, prop, withCache)
      if (typeof input !== 'string') return tokens
      for (const [word] of input.matchAll(identifierRun)) {
        const parts = identifierParts(word)
        if (parts.length < 2) continue
        tokens.push(...parts)
        for (let i = 1; i < parts.length - 1; i++)
          tokens.push(parts.slice(i).join(''))
        if (word.includes('_')) tokens.push(parts.join(''))
      }
      return tokens
    },
  }
}

/** Row is a table row, as the site's index stores it. */
interface Row {
  heading: string | undefined
  content: string
  table?: string
}

/**
 * symbolsOf returns the identifiers a table row defines, joined by commas:
 * each code span in its first cell, cut at the first `(`, so
 * `` `replaceAbility(index, name, keep?)` `` gives `replaceAbility`. It
 * returns an empty string for a row whose first cell holds no code.
 */
export function symbolsOf(row: Row): string {
  // The row's own line comes last, after the table's header and separator.
  const last = row.content.trimEnd().split('\n').at(-1) ?? ''
  const first = last.split('|')[1] ?? ''
  const symbols: string[] = []
  for (const [, code] of first.matchAll(/`([^`]+)`/g)) {
    const symbol = code.split('(')[0].trim()
    if (symbol.length > 0 && symbol.length <= 60) symbols.push(symbol)
  }
  return symbols.join(', ')
}

/**
 * searchParams are the search dialog's query options. A tolerance of 1 lets a
 * query word one typo away from an indexed word still match it, so
 * `replaceAbilty` finds `replaceAbility`.
 */
export const searchParams = { tolerance: 1 }
