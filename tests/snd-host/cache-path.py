#!/usr/bin/env python3
"""Cache v4's glob rejects literal . / .. path components, even in absolute paths."""
import pathlib, re
text = pathlib.Path('.github/workflows/release.yml').read_text()
match = re.search(r'^\s+TOOLS_DIR:\s*(.+?)\s*$', text, re.MULTILINE)
assert match, 'workflow must define TOOLS_DIR'
template = match.group(1)
assert all(part not in ('.', '..') for part in template.split('/')), 'cache path contains forbidden relative component'
assert template == '${{ github.workspace }}/.tools', 'job-level env requires github context, not runner context'
print('snd-host: workflow tools cache uses valid job context and canonical path')
