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
// on the site, `../../reference/projects/#programs`: a page is one
// deeper than its file, but an `index.md` is its directory itself, and
// `x/index.md` is `x/`.  A ```haskell block (as GitHub shows it) is
// heff's.
function pages() {
  return (tree, file) => {
    const up = /(^|\/)index\.md$/.test(file.path) ? './' : '../';
    const walk = (node) => {
      if (node.type === 'code' && node.lang === 'haskell') node.lang = 'heff';
      if (node.type === 'link' && !node.url.includes(':')) {
        const page = /^([\w./-]+?)(\/index)?\.md(#.*)?$/.exec(node.url);
        if (page) node.url = `${up}${page[1]}/${page[3] ?? ''}`;
      }
      node.children?.forEach(walk);
    };
    walk(tree);
  };
}

// The site is the repository's GitHub Pages, under /heff/.
export default defineConfig({
  site: 'https://dannypsnl.github.io',
  base: '/heff',
  markdown: { remarkPlugins: [pages] },
  integrations: [
    starlight({
      title: 'heff',
      sidebar: [
        { label: 'Tutorial', items: ['tutorial/first-program', 'tutorial/data', 'tutorial/effects', 'tutorial/own-effects', 'tutorial/terminal', 'tutorial/packages', 'tutorial/tests'] },
        {
          label: 'Reference',
          items: [
            {
              label: 'Language',
              items: [
                'reference/language/modules',
                'reference/language/definitions',
                'reference/language/data',
                'reference/language/arguments',
                'reference/language/operators',
                'reference/language/numbers',
                'reference/language/text',
                'reference/language/binaries',
                'reference/language/do',
                'reference/language/effects',
              ],
            },
            {
              label: 'Library',
              items: [
                'reference/library',
                'reference/library/collections',
                'reference/library/with',
                'reference/library/system',
                'reference/library/network',
                'reference/library/green',
                'reference/library/terminal',
                'reference/library/formats',
                'reference/library/browser',
              ],
            },
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
