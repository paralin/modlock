/**
 * languages lists the languages a mod can be written in, in the order the
 * switch shows them. The first is the one a new reader sees.
 */
export const languages = [
  { id: 'ts', name: 'TypeScript' },
  { id: 'luau', name: 'Luau' },
  { id: 'python', name: 'Python' },
  { id: 'go', name: 'Go' },
] as const

/** LanguageId names one language. */
export type LanguageId = (typeof languages)[number]['id']

/** defaultLanguage is the language shown before the reader chooses. */
export const defaultLanguage: LanguageId = languages[0].id

/** storageKey is the localStorage key that keeps the reader's choice. */
export const storageKey = 'modlock-language'

/** aliases maps a code fence's language to the language it is a sample of. */
const aliases: Record<string, LanguageId> = {
  ts: 'ts',
  tsx: 'ts',
  typescript: 'ts',
  luau: 'luau',
  lua: 'luau',
  python: 'python',
  py: 'python',
  go: 'go',
}

/** languageOf returns the language a code fence samples, if it is one. */
export function languageOf(
  fence: string | null | undefined,
): LanguageId | undefined {
  return fence ? aliases[fence] : undefined
}

/** languageName returns a language's display name. */
export function languageName(id: LanguageId): string {
  return languages.find((language) => language.id === id)?.name ?? id
}

/**
 * restoreScript runs in the document head before the first paint. It copies
 * the reader's saved choice to the root element, where the style sheet reads
 * it, so the page never draws in one language and then switches.
 */
export const restoreScript = `try{var l=localStorage.getItem(${JSON.stringify(storageKey)});if(${JSON.stringify(languages.map((language) => language.id))}.indexOf(l)>=0)document.documentElement.dataset.lang=l}catch(e){}`
