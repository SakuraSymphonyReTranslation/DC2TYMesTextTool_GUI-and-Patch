"""Read-only audit of translated JSON and DC2TY encrypted MES UI labels."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


def normalize(text):
    text = ''.join(chr(ord(c) - 0xFEE0) if 0xFF01 <= ord(c) <= 0xFF5E else c
                   for c in text)
    return re.sub(r'\s+', ' ', text).strip()


def mes_labels(folder):
    result = {"files": 0, "uiLabels": [], "choices": [], "errors": []}
    for path in sorted(folder.glob('*.mes')):
        result['files'] += 1
        data = path.read_bytes()
        try:
            count, marker = struct.unpack_from('<II', data)
            pos = count * 4 + 4 + (3 if marker == 3 else 2)
            if pos >= len(data):
                raise ValueError('Invalid bytecode start')
            while pos < len(data):
                op = data[pos]
                if op <= 0x28:
                    end = pos + 3
                elif op <= 0x4D:
                    start = pos + (2 if op <= 0x2E else 1)
                    stop = data.index(0, start)
                    end = stop + 1
                    if 0x4A <= op <= 0x4D:
                        raw = bytes((b + 0x20) & 255 for b in data[start:stop])
                        text = raw.decode('cp932')
                        item = {"file": path.name, "offset": pos,
                                "opcode": hex(op), "message": text,
                                "normalized": normalize(text)}
                        if op == 0x4D:
                            result['uiLabels'].append(item)
                        if normalize(text).startswith('\u25cb'):
                            result['choices'].append(item)
                else:
                    end = pos + 9
                if end > len(data):
                    raise ValueError(f'Truncated token at {pos}')
                pos = end
        except (ValueError, UnicodeError, struct.error) as error:
            result['errors'].append({"file": path.name, "error": str(error)})
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('json_dir', type=Path)
    parser.add_argument('original_mes', type=Path)
    parser.add_argument('active_mes', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    report = {"jsonFiles": 0, "jsonChoices": [], "jsonErrors": [], "jsonSha256": {}}
    for path in sorted(args.json_dir.glob('*.json')):
        report['jsonFiles'] += 1
        raw = path.read_bytes()
        report['jsonSha256'][path.name] = hashlib.sha256(raw).hexdigest()
        try:
            entries = json.loads(raw.decode('utf-8-sig'))
            for index, entry in enumerate(entries):
                text = entry.get('message', '')
                if text.lstrip().startswith('\u25cb'):
                    report['jsonChoices'].append({"file": path.name, "index": index,
                                                  "message": text, "length": len(text)})
        except (ValueError, UnicodeError, AttributeError) as error:
            report['jsonErrors'].append({"file": path.name, "error": str(error)})
    report['maxLength'] = max((x['length'] for x in report['jsonChoices']), default=0)
    report['originalMes'] = mes_labels(args.original_mes)
    report['activeMes'] = mes_labels(args.active_mes)
    report['allJsonChoicesMatchActiveMes'] = sorted(
        normalize(x['message']) for x in report['jsonChoices']) == sorted(
        x['normalized'] for x in report['activeMes']['choices'])
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({"jsonFiles": report['jsonFiles'],
                      "jsonChoices": len(report['jsonChoices']),
                      "maxLength": report['maxLength'],
                      "originalChoices": len(report['originalMes']['choices']),
                      "activeChoices": len(report['activeMes']['choices']),
                      "matching": report['allJsonChoicesMatchActiveMes'],
                      "errors": report['jsonErrors'] + report['originalMes']['errors'] +
                                report['activeMes']['errors']}, indent=2))


if __name__ == '__main__':
    main()
