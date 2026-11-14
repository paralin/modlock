import { createMDX } from 'fumadocs-mdx/next'

const withMDX = createMDX()

// DOCS_BASE_PATH is the path the site is served under, such as /modlock on
// GitHub Pages. It is empty when the site sits at its host's root.
const basePath = process.env.DOCS_BASE_PATH ?? ''

/** @type {import('next').NextConfig} */
const config = {
  output: 'export',
  reactStrictMode: true,
  // The site has its own lockfile inside a repository that has another.
  turbopack: { root: import.meta.dirname },
  basePath,
  env: { NEXT_PUBLIC_BASE_PATH: basePath },
}

export default withMDX(config)
