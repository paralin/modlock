'use client'

import { useStaticSearch } from 'fumadocs-core/search/client'
import {
  SearchDialog,
  SearchDialogClose,
  SearchDialogContent,
  SearchDialogHeader,
  SearchDialogIcon,
  SearchDialogInput,
  SearchDialogList,
  SearchDialogOverlay,
  type SharedProps,
} from 'fumadocs-ui/components/dialog/search'

import { basePath } from '@/lib/site'

/**
 * StaticSearchDialog searches the index built with the site. The index is one
 * static file, fetched on the first search and queried in the browser.
 */
export default function StaticSearchDialog(props: SharedProps) {
  const search = useStaticSearch({ from: `${basePath}/api/search` })
  return (
    <SearchDialog {...search} {...props}>
      <SearchDialogOverlay />
      <SearchDialogContent>
        <SearchDialogHeader>
          <SearchDialogIcon />
          <SearchDialogInput />
          <SearchDialogClose />
        </SearchDialogHeader>
        <SearchDialogList />
      </SearchDialogContent>
    </SearchDialog>
  )
}
