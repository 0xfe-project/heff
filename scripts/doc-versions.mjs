// The documentation's versions, from the release tags read on stdin
// (`git tag`, or `git ls-remote --tags`: a line each, `vX.Y.Z` or
// `refs/tags/vX.Y.Z`, after a hash or not).  Each release is a version
// of its own, `X.Y.Z`.  Tags with more than X.Y.Z (`v0.5.0-rc.1`) are not
// releases, and v0.4.0 is none of the site's: it came before the site,
// its documentation plain pages.  The newest version is the latest, the
// site's root; main is `dev`, also the root while there is no release.
//
//   node scripts/doc-versions.mjs matrix   the builds: a JSON list of
//     {version, ref, dirs}, each ref built once for each of its dirs
//     (`.` the root)
//   node scripts/doc-versions.mjs json     versions.json, which the
//     version picker reads: {latest, versions: [{name, path, tag?}]}
//   node scripts/doc-versions.mjs          the versions, a line each
import { readFileSync } from 'node:fs';

// (released before the site)
const before = new Set(['v0.4.0']);

function versions(text) {
  const found = new Map();
  for (const line of text.split('\n')) {
    const tag = line.trim().split(/\s+/).at(-1)?.replace(/^refs\/tags\//, '');
    const m = /^v((0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*))$/.exec(tag ?? '');
    if (!m || before.has(tag)) continue;
    found.set(m[1], { name: m[1], tag, key: m.slice(2).map(Number) });
  }
  return [...found.values()].sort((a, b) => compare(b.key, a.key)).map(({ name, tag }) => ({ name, tag }));
}

function compare(a, b) {
  return a[0] - b[0] || a[1] - b[1] || a[2] - b[2];
}

function matrix(vs) {
  return [
    { version: 'dev', ref: 'main', dirs: vs.length ? 'dev' : 'dev .' },
    ...vs.map((v, i) => ({ version: v.name, ref: v.tag, dirs: i === 0 ? `${v.name} .` : v.name })),
  ];
}

function json(vs) {
  return {
    latest: vs[0]?.name ?? null,
    versions: [
      { name: 'dev', path: 'dev/' },
      ...vs.map((v, i) => ({ name: v.name, path: i === 0 ? '' : `${v.name}/`, tag: v.tag })),
    ],
  };
}

const args = process.argv.slice(2);
const vs = versions(readFileSync(0, 'utf8'));
const mode = args[0];
if (mode === 'matrix') console.log(JSON.stringify(matrix(vs)));
else if (mode === 'json') console.log(JSON.stringify(json(vs)));
else for (const v of vs) console.log(v.name, v.tag);
