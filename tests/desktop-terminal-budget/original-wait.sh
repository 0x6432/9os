 pty_ready=0;terminal_ready=0;deadline=$(( $(date +%s) + 120 ))
 while [ "$(date +%s)" -lt "$deadline" ];do
  if grep -q '^/dev/pts/' "$D/interactive-pty" 2>/dev/null && grep -qx KDE_INTERACTIVE_PTY "$D/interactive-pty";then
   pty_ready=1
   if timeout -k 2 -s TERM 8 desktopa11y --terminal "$client" > "$D/konsole-focus" 2>&1;then terminal_ready=1;break;fi
  fi
  kill -0 "$client" 2>/dev/null || break
  sleep .1
 done
