// The documentation's versions, from the release tags read on stdin
// (`git tag`, or `git ls-remote --tags`: a line each, `vX.Y.Z` or
// `refs/tags/vX.Y.Z`, after a hash or not).  A version is the newest
// tag of each compatibility unit: under 0.x a minor, `0.N`, then a
// major, `N`.  Tags with more than X.Y.Z (`v0.5.0-rc.1`) are not
// releases.  With `--site`, a tag is a version only if it has the site
// (docs/astro.config.mjs, asked of git: the tags must be fetched); the
// older ones' documentation was plain pages.  The newest version is the latest, the site's root; main is
// `dev`, also the root while there is no release.
//
//   node scripts/doc-versions.mjs matrix   the builds: a JSON list of
//     {version, ref, dirs}, each ref built once for each of its dirs
//     (`.` the root)
//   node scripts/doc-versions.mjs json     versions.json, which the
//     version picker reads: {latest, versions: [{name, path, tag?}]}
//   node scripts/doc-versions.mjs          the versions, a line each
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';

function site(tag) {
  try {
    execFileSync('git', ['cat-file', '-e', `${tag}:docs/astro.config.mjs`], { stdio: 'ignore' });
    return true;
  } catch {
    return false;
  }
}

function versions(text, withSite) {
  const newest = new Map();
  for (const line of text.split('\n')) {
    const tag = line.trim().split(/\s+/).at(-1)?.replace(/^refs\/tags\//, '');
    const m = /^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.exec(tag ?? '');
    if (!m || (withSite && !site(tag))) continue;
    const [major, minor, patch] = m.slice(1).map(Number);
    const name = major === 0 ? `0.${minor}` : `${major}`;
    const key = [major, minor, patch];
    const old = newest.get(name);
    if (!old || compare(key, old.key) > 0) newest.set(name, { name, tag, key });
  }
  return [...newest.values()].sort((a, b) => compare(b.key, a.key)).map(({ name, tag }) => ({ name, tag }));
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
const vs = versions(readFileSync(0, 'utf8'), args.includes('--site'));
const mode = args.find((a) => a !== '--site');
if (mode === 'matrix') console.log(JSON.stringify(matrix(vs)));
else if (mode === 'json') console.log(JSON.stringify(json(vs)));
else for (const v of vs) console.log(v.name, v.tag);
