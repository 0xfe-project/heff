// The documentation's site: the pages are the .md files of tutorial/ and
// reference/, as they are read on GitHub, and the library's API pages,
// api/, made by `make api` (not kept: the workflow makes them before
// each build); `npm run build` makes the site in dist/.  Each version
// of the site is a build of its own, at its own base, which the
// environment gives.
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import { base } from './src/site.mjs';

const heff = JSON.parse(
  readFileSync(new URL('../editors/vscode/syntaxes/heff.tmLanguage.json', import.meta.url), 'utf8'),
);

// A GitHub alert, `> [!CAUTION]` and its lines, is a Starlight aside.
const asides = { NOTE: 'note', TIP: 'tip', IMPORTANT: 'note', WARNING: 'caution', CAUTION: 'danger' };
function aside(node) {
  const text = node.children?.[0]?.children?.[0];
  const alert = text?.type === 'text' && /^\[!(\w+)\]\s*/.exec(text.value);
  if (!alert || !asides[alert[1]]) return;
  const kind = asides[alert[1]];
  text.value = text.value.slice(alert[0].length);
  node.data = { hName: 'aside', hProperties: { 'aria-label': alert[1][0] + alert[1].slice(1).toLowerCase(), className: ['starlight-aside', `starlight-aside--${kind}`] } };
  node.children = [{ type: 'paragraph', data: { hName: 'p', hProperties: { className: ['starlight-aside__title'] } }, children: [{ type: 'text', value: alert[1][0] + alert[1].slice(1).toLowerCase() }] },
    { type: 'blockquote', data: { hName: 'div', hProperties: { className: ['starlight-aside__content'] } }, children: node.children }];
}

// A link to a page, `../reference/projects.md#programs`, is to its page
// on the site, `/heff/reference/projects/#programs`: the file it names,
// from the file it is in, without `.md`, and `x/index.md` is `x/`.  It
// is written from the root, so it is the same page whether the page it
// is on is asked for with its last `/` or without.  A ```haskell block
// (as GitHub shows it) is heff's.  A heading's `{#id}`, at its end, is
// its id (the API pages give each definition's so, its name).
const root = fileURLToPath(new URL('.', import.meta.url));
function pages() {
  return (tree, file) => {
    const dir = path.posix.dirname(path.relative(root, file.path).split(path.sep).join('/'));
    const walk = (node) => {
      if (node.type === 'code' && node.lang === 'haskell') node.lang = 'heff';
      if (node.type === 'blockquote') aside(node);
      const last = node.type === 'heading' ? node.children.at(-1) : null;
      const id = last?.type === 'text' && / \{#([^}]+)\}$/.exec(last.value);
      if (id) {
        last.value = last.value.slice(0, -id[0].length);
        node.data = { ...node.data, hProperties: { ...node.data?.hProperties, id: id[1] } };
      }
      if (node.type === 'link' && !node.url.includes(':')) {
        const page = /^([\w./-]+?)(\/?index)?\.md(#.*)?$/.exec(node.url);
        if (page) {
          const to = path.posix.normalize(path.posix.join(dir, page[1]));
          node.url = `${base}/${to === '.' ? '' : `${to}/`}${page[3] ?? ''}`;
        }
      }
      node.children?.forEach(walk);
    };
    walk(tree);
  };
}

export default defineConfig({
  site: 'https://dannypsnl.github.io',
  base,
  markdown: { remarkPlugins: [pages] },
  integrations: [
    starlight({
      title: 'heff',
      // (the version picker beside the title; main's notice above each page)
      components: { SiteTitle: './src/components/SiteTitle.astro', Banner: './src/components/Banner.astro' },
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
                'reference/library/resource',
                'reference/library/system',
                'reference/library/network',
                'reference/library/concurrency',
                'reference/library/terminal',
                'reference/library/formats',
                'reference/library/browser',
              ],
            },
            'reference/projects',
            'reference/commands',
            'reference/tests',
            'reference/benchmarks',
            'reference/providers',
          ],
        },
        { label: 'API', items: [{ autogenerate: { directory: 'api' } }] },
      ],
      expressiveCode: { shiki: { langs: [heff] } },
    }),
  ],
});
