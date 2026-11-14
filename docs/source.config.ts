import { defineConfig } from 'fumadocs-mdx/config'

import { remarkSamples } from './lib/remark-samples'

export default defineConfig({
  mdxOptions: { remarkPlugins: [remarkSamples] },
})
