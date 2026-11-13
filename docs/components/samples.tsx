import type { ReactNode } from 'react'

import { type LanguageId, languageName, languages } from '@/lib/language'

import { LanguageButtons } from './language'

/**
 * Samples shows the same code in each language a mod can be written in, one
 * at a time, following the reader's language. In MDX it wraps plain code
 * fences; remarkSamples turns each into a Sample and fills in langs.
 *
 * A block that lacks the reader's language shows its first sample and says so.
 */
export function Samples({
  langs = '',
  children,
}: {
  langs?: string
  children: ReactNode
}) {
  const held = langs.split(' ').filter(Boolean) as LanguageId[]
  const first = held[0]
  return (
    <div data-samples="" data-langs={langs}>
      {held.length > 1 && <LanguageButtons ids={held} />}
      {first !== undefined &&
        languages
          .filter((language) => !held.includes(language.id))
          .map((language) => (
            <p key={language.id} data-missing={language.id}>
              {held.length === 1
                ? `This is ${languageName(first)} only.`
                : `No ${language.name} sample yet. Showing ${languageName(first)}.`}
            </p>
          ))}
      {children}
    </div>
  )
}

/** Sample is one language's code inside Samples. */
export function Sample({
  lang,
  children,
}: {
  lang: LanguageId
  children: ReactNode
}) {
  return <section data-sample={lang}>{children}</section>
}

/**
 * Only shows its children when the reader's language is one of langs, a
 * space-separated list, for a note that applies to some languages.
 */
export function Only({
  lang,
  children,
}: {
  lang: string
  children: ReactNode
}) {
  return <div data-only={lang}>{children}</div>
}
