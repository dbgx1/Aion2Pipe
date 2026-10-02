"""Inventory decoded structures and remaining work without exposing field values."""
import argparse
import collections
import json
import pathlib


def summarize(path):
    totals = collections.Counter()
    unknown = collections.Counter()
    uncertain = collections.Counter()

    def visit(message, scope):
        totals[scope] += 1
        if message.get('structure_complete'):
            totals[scope + '_complete'] += 1
        else:
            unknown[(scope, message.get('name', '').split(' ')[0])] += 1
        for field in message.get('fields', []):
            if not field.get('meaning_known'):
                uncertain[(message.get('name', '').split(' ')[0], field['name'])] += 1
        for child in message.get('children', []):
            if 'fields' in child:
                visit(child, 'inner')
            else:
                totals['truncated_reports'] += 1

    with path.open(encoding='utf8') as report:
        for line in report:
            row = json.loads(line)
            if row['kind'] == 'capture':
                totals['packet_count'] = row['packet_count']
                totals['outside_profile_packets'] = row['outside_profile_packets']
            elif row['kind'] == 'stream':
                totals['streams'] += 1
                totals['streams_with_gaps'] += bool(row['gap'])
            elif row['kind'] == 'message':
                visit(row, 'inbound' if row['inbound_by_port'] else 'outbound')
    return {
        'source': str(path), 'counts': dict(totals),
        'incomplete_structures': [{'scope': key[0], 'opcode': key[1], 'count': value}
                                  for key, value in unknown.most_common()],
        'uncertain_field_meanings': [{'opcode': key[0], 'field': key[1], 'occurrences': value}
                                    for key, value in uncertain.most_common()],
        'note': 'Structure completeness does not establish business semantics.',
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('report', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    args = parser.parse_args()
    result = summarize(args.report)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf8')
    print(json.dumps(result['counts']))
