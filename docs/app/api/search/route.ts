import { createFromSource } from 'fumadocs-core/search/server'

import { source } from '@/lib/source'

export const revalidate = false

/** GET serves the search index, built once with the site. */
export const { staticGET: GET } = createFromSource(source, {
  language: 'english',
})
