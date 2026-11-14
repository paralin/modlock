'use client'

import { useSyncExternalStore } from 'react'

import {
  defaultLanguage,
  type LanguageId,
  languages,
  storageKey,
} from '@/lib/language'

// The reader's language lives on the root element as data-lang, set before
// the first paint by restoreScript. The style sheet reads it to show one
// sample of each block, and this store reads it for the controls' state.

const listeners = new Set<() => void>()

/** current reads the language from the root element. */
function current(): LanguageId {
  const value = document.documentElement.dataset.lang
  return (
    languages.find((language) => language.id === value)?.id ?? defaultLanguage
  )
}

/** apply writes the language to the root element and wakes the controls. */
function apply(id: LanguageId): void {
  document.documentElement.dataset.lang = id
  for (const listener of listeners) {
    listener()
  }
}

/** onStorage follows a choice made in another tab. */
function onStorage(event: StorageEvent): void {
  if (event.key !== storageKey) {
    return
  }
  const next = languages.find((language) => language.id === event.newValue)
  if (next) {
    apply(next.id)
  }
}

function subscribe(listener: () => void): () => void {
  if (listeners.size === 0) {
    window.addEventListener('storage', onStorage)
  }
  listeners.add(listener)
  return () => {
    listeners.delete(listener)
    if (listeners.size === 0) {
      window.removeEventListener('storage', onStorage)
    }
  }
}

/** useLanguage returns the reader's language and follows its changes. */
export function useLanguage(): LanguageId {
  return useSyncExternalStore(subscribe, current, () => defaultLanguage)
}

/**
 * setLanguage switches every sample on the page to a language and remembers
 * it. Samples differ in height, so the page keeps anchor, the control the
 * reader pressed, at the same place in the window.
 */
export function setLanguage(id: LanguageId, anchor?: Element): void {
  const before = anchor?.getBoundingClientRect().top
  apply(id)
  try {
    localStorage.setItem(storageKey, id)
  } catch {
    // Storage is blocked; the choice lasts for this page.
  }
  if (anchor && before !== undefined) {
    window.scrollBy({
      top: anchor.getBoundingClientRect().top - before,
      behavior: 'instant',
    })
  }
}

/**
 * LanguageButtons draws one button for each of the given languages. The style
 * sheet marks the chosen one from the root element, so it is right on the
 * first paint; the pressed state for assistive technology follows after load.
 */
export function LanguageButtons({
  ids,
  className,
}: {
  ids?: readonly LanguageId[]
  className?: string
}) {
  const language = useLanguage()
  const shown = ids
    ? languages.filter((entry) => ids.includes(entry.id))
    : languages
  return (
    <div
      role="group"
      aria-label="Language"
      className={className}
      data-language-buttons=""
    >
      {shown.map((entry) => (
        <button
          key={entry.id}
          type="button"
          data-language={entry.id}
          aria-pressed={language === entry.id}
          onClick={(event) => setLanguage(entry.id, event.currentTarget)}
        >
          {entry.name}
        </button>
      ))}
    </div>
  )
}

/** LanguageSelect is the site-wide language switch in the sidebar. */
export function LanguageSelect() {
  return (
    <div className="language-select">
      <span className="language-select-label">Samples in</span>
      <LanguageButtons />
    </div>
  )
}
