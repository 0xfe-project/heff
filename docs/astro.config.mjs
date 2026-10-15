// The documentation's site: the pages are the .md files of tutorial/ and
// reference/, as they are read on GitHub; `npm run build` makes the site
// in dist/.
import { readFileSync } from 'node:fs';
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

const heff = JSON.parse(
  readFileSync(new URL('../editors/vscode/syntaxes/heff.tmLanguage.json', import.meta.url), 'utf8'),
);

// A link to a page, `../reference/projects.md#programs`, is to its page
// on the site, one deeper, `../../reference/projects/#programs`; a
// ```haskell block (as GitHub shows it) is heff's.
function pages() {
  const walk = (node) => {
    if (node.type === 'code' && node.lang === 'haskell') node.lang = 'heff';
    if (node.type === 'link') {
      const page = /^([\w./-]+)\.md(#.*)?$/.exec(node.url);
      if (page && !node.url.includes(':')) node.url = `../${page[1]}/${page[2] ?? ''}`;
    }
    node.children?.forEach(walk);
  };
  return walk;
}

export default defineConfig({
  markdown: { remarkPlugins: [pages] },
  integrations: [
    starlight({
      title: 'heff',
      sidebar: [
        { label: 'Tutorial', items: ['tutorial/first-program', 'tutorial/data', 'tutorial/effects'] },
        {
          label: 'Reference',
          items: [
            'reference/language',
            'reference/library',
            'reference/threads',
            'reference/projects',
            'reference/commands',
            'reference/tests',
            'reference/benchmarks',
            'reference/providers',
          ],
        },
      ],
      expressiveCode: { shiki: { langs: [heff] } },
    }),
  ],
});
