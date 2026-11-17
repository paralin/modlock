import type { StructuredData } from 'fumadocs-core/mdx-plugins'
import { createFromSource } from 'fumadocs-core/search/server'

import { searchTokenizer, symbolsOf } from '@/lib/search'
import { source } from '@/lib/source'

export const revalidate = false

/**
 * GET serves the search index, built once with the site. The identifiers a
 * table row defines, an API call or an event, also become a short entry of
 * their own that links to the row's section, a heading entry where the row
 * has a section, so a search for the name lists it above the prose that only
 * mentions it and shows which name matched.
 */
export const { staticGET: GET } = createFromSource(source, {
  tokenizer: searchTokenizer(),
  buildIndex(page) {
    // Add a short entry for the names each table row defines.
    const data: StructuredData = page.data.structuredData
    const headings = [...data.headings]
    const contents = [...data.contents]
    for (const row of data.contents) {
      const symbols = row.table ? symbolsOf(row) : ''
      if (!symbols) continue
      if (row.heading) headings.push({ id: row.heading, content: symbols })
      else contents.push({ heading: undefined, content: symbols })
    }
    return {
      title: page.data.title,
      description: page.data.description,
      url: page.url,
      id: page.url,
      structuredData: { headings, contents },
    }
  },
})
