# Auxo zsh prompt: accent-coloured, git-aware, shows exit status
_auxo_accent() { local a; a=$(sed -n 's/^ACCENT="\{0,1\}\([a-z]*\).*/\1/p' /etc/auxo/auxo.conf 2>/dev/null)
  case $a in cyan) echo 51;; emerald) echo 79;; amber) echo 214;; rose) echo 211;; blue) echo 75;; mono) echo 254;; *) echo 141;; esac; }
AUXO_C=$(_auxo_accent)
autoload -Uz vcs_info add-zsh-hook
zstyle ':vcs_info:git:*' formats ' %F{245}on%f %F{'$AUXO_C'}%b%f%u%c'
zstyle ':vcs_info:*' check-for-changes true
zstyle ':vcs_info:*' unstagedstr '%F{214}*%f'
zstyle ':vcs_info:*' stagedstr '%F{79}+%f'
add-zsh-hook precmd vcs_info
setopt prompt_subst
PROMPT='%F{'$AUXO_C'}▲%f %B%~%b${vcs_info_msg_0_} %(?.%F{'$AUXO_C'}.%F{203})❯%f '
RPROMPT='%(?..%F{203}%?%f)'
