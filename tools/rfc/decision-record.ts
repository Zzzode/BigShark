const historicalApprovals = new Set(['0001', '0002', '0003']);

export function validateDecisionRecord(id: string, status: string, decision: string): string[] {
  if (status === 'Draft' || status === 'Proposed') {
    return decision.includes('Pending independent approval-agent review.')
      ? []
      : [`${status} RFC must state pending independent approval-agent review`];
  }
  if (!['Accepted', 'Implementing', 'Implemented', 'Rejected'].includes(status)) return [];

  const errors: string[] = [];
  const fields = new Map<string, string>();
  for (const line of decision.split('\n')) {
    const match = line.match(/^([^:\n]+):[ \t]*(.*)$/);
    if (!match?.[1] || match[2] === undefined) continue;
    const key = match[1].trim();
    if (fields.has(key)) errors.push(`duplicate decision field ${key}`);
    fields.set(key, match[2].trim());
  }
  const rejected = status === 'Rejected';
  const approverKey = rejected ? 'Rejected by' : 'Approved by';
  for (const key of [approverKey, 'Decision date']) {
    if (!fields.get(key)) errors.push(`${status} RFC must record ${key}`);
  }
  if (!/^\d{4}-\d{2}-\d{2}$/.test(fields.get('Decision date') ?? '')) {
    errors.push('Decision date must use YYYY-MM-DD');
  }
  if (fields.has(rejected ? 'Approved by' : 'Rejected by')) {
    errors.push('decision must not contain conflicting approval and rejection fields');
  }
  const historical = !rejected && historicalApprovals.has(id)
    && fields.get('Approved by') === 'BigShark project maintainer'
    && fields.get('Decision date') === '2026-09-13'
    && !fields.has('Author agent') && !fields.has('Review outcome');
  if (historical) return errors;

  for (const key of ['Author agent', 'Reviewed scope', 'Review summary']) {
    if (!fields.get(key)) errors.push(`${status} RFC must record ${key}`);
  }
  const outcome = rejected ? 'Rejected' : 'Approved';
  if (fields.get('Review outcome') !== outcome) {
    errors.push(`${status} RFC requires Review outcome: ${outcome}`);
  }
  if (fields.get('Author agent') && fields.get('Author agent') === fields.get(approverKey)) {
    errors.push('approval agent must differ from author agent');
  }
  return errors;
}
