import { defineCollection } from 'astro:content';
import { glob } from 'astro/loaders';
import { docsSchema } from '@astrojs/starlight/schema';

// The pages are docs/'s own .md files, not src/content/docs/'s, and the
// library's API pages, api/, which `make api` makes (`heff doc`).
export const collections = {
  docs: defineCollection({ loader: glob({ pattern: ['index.md', 'tutorial/**/*.md', 'reference/**/*.md', 'api/**/*.md'], base: '.' }), schema: docsSchema() }),
};
