# Auxo bash prompt
case $(sed -n 's/^ACCENT="\{0,1\}\([a-z]*\).*/\1/p' /etc/auxo/auxo.conf 2>/dev/null) in
  cyan) _c=51;; emerald) _c=79;; amber) _c=214;; rose) _c=211;; blue) _c=75;; mono) _c=254;; *) _c=141;;
esac
PS1="\[\e[38;5;${_c}m\]▲\[\e[0m\] \[\e[1m\]\w\[\e[0m\] \[\e[38;5;${_c}m\]❯\[\e[0m\] "
