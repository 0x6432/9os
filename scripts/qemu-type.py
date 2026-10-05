# Type text into the guest through the QEMU monitor (sendkey). usage: qemu-type.py SOCK TEXT...
# "\n" in TEXT becomes ret; each argument is followed by nothing (use \n explicitly).
import socket, sys, time
M = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '/': 'slash', '.': 'dot', ',': 'comma', ';': 'semicolon',
     "'": 'apostrophe', '|': 'shift-backslash', '\\': 'backslash', '>': 'shift-dot', '<': 'shift-comma', '_': 'shift-minus',
     '$': 'shift-4', '&': 'shift-7', '*': 'shift-8', '"': 'shift-apostrophe', ':': 'shift-semicolon', '~': 'shift-grave_accent',
     '!': 'shift-1', '(': 'shift-9', ')': 'shift-0', '[': 'bracket_left', ']': 'bracket_right', '+': 'shift-equal'}
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(0.2); s.setblocking(False)
for text in sys.argv[2:]:
    for ch in text.encode().decode('unicode_escape'):
        k = M.get(ch) or (('shift-' + ch.lower()) if ch.isupper() else ch)
        s.send(('sendkey %s\n' % k).encode()); time.sleep(0.08)
        try: s.recv(65536)
        except BlockingIOError: pass
s.close()
