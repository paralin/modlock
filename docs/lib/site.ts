/** siteName is the product's name, shown in the navigation and page titles. */
export const siteName = 'Modlock'

/** siteDescription is the one-line summary search engines show. */
export const siteDescription =
  'Custom game modes for Deadlock, in the language you like.'

/** basePath is the path the site is served under, without a trailing slash. */
export const basePath = process.env.NEXT_PUBLIC_BASE_PATH ?? ''

/** repository locates the source on GitHub. */
export const repository = {
  user: 'paralin',
  repo: 'modlock',
  branch: 'master',
  /** contentDir is the content directory's path in the repository. */
  contentDir: 'docs/content',
}

/** repositoryUrl is the repository's page on GitHub. */
export const repositoryUrl = `https://github.com/${repository.user}/${repository.repo}`

/** markdownRoute is where each page's Markdown source is served. */
export const markdownRoute = '/llms.mdx'

/** markdownSegments returns the path of a page's Markdown under markdownRoute. */
export function markdownSegments(page: { slugs: string[] }): string[] {
  return [...page.slugs, 'content.md']
}
