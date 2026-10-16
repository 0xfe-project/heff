// Where this build of the site is, from the environment the workflow
// gives each version's build: DOCS_ROOT, the site's root, where all the
// versions are and their list, versions.json; DOCS_BASE, this build's
// own place under it; DOCS_VERSION, its name (`dev` for main).  Without
// them it is main's at the root, as a build on one's own machine is.
const trim = (path) => path.replace(/\/+$/, '');

export const root = trim(process.env.DOCS_ROOT ?? '/heff');
export const base = trim(process.env.DOCS_BASE ?? root);
export const version = process.env.DOCS_VERSION ?? 'dev';
