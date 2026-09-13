#!/usr/bin/env node

import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { basename, dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const rfcDirectory = join(root, 'docs', 'rfcs');
const indexPath = join(rfcDirectory, 'README.md');
const rfcName = /^(\d{4})-([a-z0-9]+(?:-[a-z0-9]+)*)\.md$/;
const allowedStatuses = new Set([
  'Draft',
  'Proposed',
  'Accepted',
  'Implementing',
  'Implemented',
  'Rejected',
  'Superseded',
]);
const requiredMetadata = [
  'rfc',
  'subject',
  'status',
  'authors',
  'created',
  'updated',
  'owners',
  'supersedes',
  'superseded-by',
];
const requiredSections = [
  'Summary',
  'Motivation',
  'Goals',
  'Non-Goals',
  'Current State and Evidence',
  'Design Principles',
  'Proposed Design',
  'Dependency Rules',
  'Compatibility and Migration',
  'Security and Operational Impact',
  'Alternatives Considered',
  'Risks',
  'Verification Plan',
  'Rollout Plan',
  'Rollback Plan',
  'Open Questions',
  'Acceptance Criteria',
  'Decision',
];

interface ParsedRfc {
  metadata: Map<string, string>;
  body: string;
}

function parseFrontMatter(text: string, file: string): ParsedRfc {
  if (!text.startsWith('---\n')) {
    throw new Error(`${file}: missing YAML front matter`);
  }
  const end = text.indexOf('\n---\n', 4);
  if (end < 0) throw new Error(`${file}: unterminated YAML front matter`);

  const metadata = new Map<string, string>();
  for (const [index, line] of text.slice(4, end).split('\n').entries()) {
    if (!line.trim()) continue;
    const match = line.match(/^([a-z][a-z0-9-]*):\s*"(.*)"$/);
    if (!match) {
      throw new Error(`${file}:${index + 2}: metadata must use quoted scalar values`);
    }
    const key = match[1];
    const value = match[2];
    if (key === undefined || value === undefined) {
      throw new Error(`${file}:${index + 2}: invalid metadata`);
    }
    if (metadata.has(key)) {
      throw new Error(`${file}:${index + 2}: duplicate metadata key ${key}`);
    }
    metadata.set(key, value);
  }
  return { metadata, body: text.slice(end + 5).trimStart() };
}

function sections(body: string): string[] {
  return [...body.matchAll(/^## (.+)$/gm)]
    .map(match => match[1]?.trim())
    .filter((title): title is string => Boolean(title));
}

function sectionBody(body: string, title: string): string {
  const marker = `## ${title}`;
  const start = body.indexOf(marker);
  if (start < 0) return '';
  const contentStart = start + marker.length;
  const next = body.indexOf('\n## ', contentStart);
  return body.slice(contentStart, next < 0 ? body.length : next).trim();
}

function date(value: string): boolean {
  return /^\d{4}-\d{2}-\d{2}$/.test(value);
}

const errors: string[] = [];
const index = readFileSync(indexPath, 'utf8');
const ids = new Set<string>();
const files = readdirSync(rfcDirectory)
  .filter(name => rfcName.test(name) && !name.startsWith('0000-'))
  .sort();

for (const name of files) {
  const file = relative(root, join(rfcDirectory, name));
  const match = name.match(rfcName);
  if (!match?.[1]) {
    errors.push(`${file}: invalid RFC filename`);
    continue;
  }
  const id = match[1];
  const text = readFileSync(join(root, file), 'utf8');

  try {
    const { metadata, body } = parseFrontMatter(text, file);
    for (const key of requiredMetadata) {
      if (!metadata.has(key)) errors.push(`${file}: missing metadata key ${key}`);
    }
    if (metadata.get('rfc') !== id) {
      errors.push(`${file}: metadata rfc must match filename ${id}`);
    }
    if (ids.has(id)) errors.push(`${file}: duplicate RFC number ${id}`);
    ids.add(id);

    const status = metadata.get('status') || '';
    if (!allowedStatuses.has(status)) {
      errors.push(`${file}: unsupported status ${status}`);
    }
    if (!date(metadata.get('created') || '')) {
      errors.push(`${file}: created must use YYYY-MM-DD`);
    }
    if (!date(metadata.get('updated') || '')) {
      errors.push(`${file}: updated must use YYYY-MM-DD`);
    }
    if ((metadata.get('updated') || '') < (metadata.get('created') || '')) {
      errors.push(`${file}: updated precedes created`);
    }
    if (!(metadata.get('authors') || '').trim()) {
      errors.push(`${file}: authors must not be empty`);
    }
    if (!(metadata.get('owners') || '').trim()) {
      errors.push(`${file}: owners must not be empty`);
    }
    if (status === 'Superseded' && !(metadata.get('superseded-by') || '').trim()) {
      errors.push(`${file}: Superseded RFC must name superseded-by`);
    }

    const expectedHeading = `# RFC ${id}: ${metadata.get('subject')}`;
    if (!body.startsWith(`${expectedHeading}\n`)) {
      errors.push(`${file}: first heading must be "${expectedHeading}"`);
    }

    const actualSections = sections(body);
    for (const required of requiredSections) {
      if (!actualSections.includes(required)) {
        errors.push(`${file}: missing section "${required}"`);
      } else if (!sectionBody(body, required)) {
        errors.push(`${file}: empty section "${required}"`);
      }
    }
    if (new Set(actualSections).size !== actualSections.length) {
      errors.push(`${file}: duplicate level-two section`);
    }

    const decision = sectionBody(body, 'Decision');
    if (['Draft', 'Proposed'].includes(status)
      && !decision.includes('Pending explicit maintainer approval.')) {
      errors.push(`${file}: ${status} RFC must state pending explicit maintainer approval`);
    }
    if (['Accepted', 'Implementing', 'Implemented'].includes(status)
      && (!decision.includes('Approved by:') || !decision.includes('Decision date:'))) {
      errors.push(`${file}: ${status} RFC must record approver and decision date`);
    }
    if (status === 'Rejected'
      && (!decision.includes('Rejected by:') || !decision.includes('Decision date:'))) {
      errors.push(`${file}: Rejected RFC must record rejector and decision date`);
    }
  } catch (error) {
    errors.push(error instanceof Error ? error.message : String(error));
  }

  if (!index.includes(`](${name})`)) {
    errors.push(`${file}: missing from docs/rfcs/README.md index`);
  }
}

const templatePath = join(rfcDirectory, '0000-template.md');
if (!existsSync(templatePath)) {
  errors.push('docs/rfcs/0000-template.md: missing RFC template');
}

if (errors.length) {
  for (const error of errors) process.stderr.write(`${error}\n`);
  process.exit(1);
}

process.stdout.write(`RFC checks passed (${files.length} RFCs).\n`);
