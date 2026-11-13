import { llms, loader } from 'fumadocs-core/source'
import { metaSchema, pageSchema } from 'fumadocs-core/source/schema'
import { defineDocs } from 'fumadocs-mdx/macro'

const docs = defineDocs({
  dir: 'content',
  docs: {
    schema: pageSchema,
    postprocess: { includeProcessedMarkdown: true },
  },
  meta: { schema: metaSchema },
})

/** source is the documentation's pages and their navigation tree. */
export const source = loader({
  baseUrl: '/',
  source: docs.toFumadocsSource(),
})

/** docsLlms renders the pages as plain Markdown for language models. */
export const docsLlms = llms(source, {
  renderPage: async (page) =>
    `# ${page.data.title} (${page.url})\n\n${await page.data.getText('processed')}`,
})
