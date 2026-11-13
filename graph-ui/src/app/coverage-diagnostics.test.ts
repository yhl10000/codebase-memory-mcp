import { describe, expect, it } from 'vitest';
import { buildCoverageIndex, mergeCoverageIntoLevels, readCoverageAnswer, readIndexStatusCoverage, readTreeLevel } from './tree-model';
import { coverageTooltip, partialFileNote } from './coverage-strings';

const path = 'tests/test_grammar_probe_d.c';
const detail = JSON.stringify(Array.from({ length: 160 }, () => ({
    caller: 'codebase-memory.tests.test_grammar_probe_d.gpd_open_indexed',
    leaf: 'cbm_project_name_from_path', start_byte: 0, end_byte: 0, source_origin: 0,
    reason: 'function_not_in_registry',
})));
const calls = { path, kind: 'unresolved_calls', detail };
const parse = { path, kind: 'parse_partial', ranges: [{ start: 66, end: 85 }, { start: 99, end: 99 }] };
const unusable = { path, kind: 'parse_unusable', detail: 'No usable syntax tree' };

function joined(entries: unknown[], withStatus = false) {
    const scopes = readCoverageAnswer({ scopes: [{ scope: '.', status: 'known_gaps', total: entries.length, has_more: false, entries }] }).scopes;
    return buildCoverageIndex({
        scopes,
        ...(withStatus ? { status: readIndexStatusCoverage({ parse_partial: { files: [{ path, error_ranges: '66-85,99-99' }], count: 1 } }) } : {}),
    });
}

describe('coverage diagnostic evidence and reader copy', () => {
    it('keeps real unresolved-only evidence out of parser ranges and visible copy', () => {
        const index = joined([calls]);
        const record = index.records.get(path)!;
        expect(record.state).toBe('partial');
        expect(record.reason).toBe('');
        expect(record.diagnostics).toEqual({ unresolvedCalls: true });
        const note = partialFileNote(path, record.reason, record.diagnostics);
        const tooltip = coverageTooltip(record.state, record.reason, record.diagnostics);
        for (const text of [note, tooltip]) {
            expect(text).toContain('Call graph incomplete');
            expect(text).not.toMatch(/partially parsed|inside lines|flagged ranges|"caller"|function_not_in_registry/);
            expect(text.length).toBeLessThan(350);
        }
        const levels = new Map([['tests', readTreeLevel({ path: 'tests', children: [{ name: 'test_grammar_probe_d.c', path, kind: 'file', files: 1, symbols: 80 }] })]]);
        const row = mergeCoverageIntoLevels(levels, index).get('tests')!.children[0]!;
        expect(row.coverageDiagnostics).toEqual({ unresolvedCalls: true });
        expect(row.coverageReason ?? '').not.toContain('"caller"');
    });

    it.each([false, true])('preserves parser ranges and call warnings in either row order, status=%s', (withStatus) => {
        for (const entries of [[calls, parse], [parse, calls]]) {
            const record = joined(entries, withStatus).records.get(path)!;
            expect(record.state).toBe('partial');
            expect(record.diagnostics).toEqual({ parsePartial: true, unresolvedCalls: true });
            for (const text of [partialFileNote(path, record.reason, record.diagnostics), coverageTooltip(record.state, record.reason, record.diagnostics)]) {
                expect(text).toContain('66-85');
                expect(text).toContain('99-99');
                expect(text).toContain('Call graph incomplete');
                expect(text).not.toContain('"caller"');
                expect(text).not.toContain('Source lines:');
            }
        }
    });

    it('does not describe an unusable parse as a few missing constructs or a complete parse', () => {
        for (const entries of [[unusable], [calls, parse, unusable], [unusable, parse, calls]]) {
            const record = joined(entries).records.get(path)!;
            expect(record.state).toBe('partial');
            expect(record.diagnostics?.parseUnusable).toBe(true);
            const text = partialFileNote(path, record.reason, record.diagnostics);
            expect(text).toContain('Parsing unavailable');
            expect(text).toContain('Symbols and relationships may be missing throughout this file');
            expect(text).not.toMatch(/only partially parsed|constructs inside lines|parse complete|"caller"/);
            if (entries.length > 1) {
                expect(text).toContain('66-85');
                expect(text).toContain('Call graph incomplete');
            }
        }
    });

    it.each(['extract', 'oversized', 'unknown_phase'])('keeps %s unreadable even alongside graph diagnostics', (kind) => {
        for (const entries of [[calls, { path, kind, detail: 'source skipped' }], [{ path, kind, detail: 'source skipped' }, calls]]) {
            const record = joined(entries).records.get(path)!;
            expect(record.state).toBe('skipped');
            expect(record.reason).toBe('source skipped');
            expect(coverageTooltip(record.state, record.reason, record.diagnostics)).toContain('skipped by the indexer');
        }
    });

    it('never treats arbitrary JSON as source line ranges, including legacy callers', () => {
        expect(partialFileNote(path, detail)).not.toContain('"caller"');
        expect(coverageTooltip('partial', detail)).not.toContain('"caller"');
    });

    it.each(['99-66', '9007199254740992', '1-9007199254740992'])('does not present invalid parser range %s as a line location', (range) => {
        const text = partialFileNote(path, range);
        expect(text).not.toContain(range);
        expect(text).toContain('without usable line ranges');
    });

    it('bounds a long list of real ranges without losing the parse warning', () => {
        const record = joined([{ path, kind: 'parse_partial', ranges: Array.from({ length: 200 }, (_, i) => ({ start: i * 2 + 1, end: i * 2 + 1 })) }]).records.get(path)!;
        const text = partialFileNote(path, record.reason, record.diagnostics);
        expect(text).toContain('partially parsed');
        expect(text).toContain('1-1');
        expect(text).toContain('Coverage');
        expect(text.length).toBeLessThan(450);
    });
});
