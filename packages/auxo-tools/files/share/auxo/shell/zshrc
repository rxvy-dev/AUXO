# ~/.zshrc — Auxo defaults (safe to edit)
HISTFILE=~/.zsh_history HISTSIZE=10000 SAVEHIST=10000
setopt autocd extendedglob hist_ignore_dups share_history
bindkey -e
autoload -Uz compinit && compinit
zstyle ':completion:*' menu select
[[ -r /usr/share/zsh/plugins/zsh-autosuggestions/zsh-autosuggestions.zsh ]] && source /usr/share/zsh/plugins/zsh-autosuggestions/zsh-autosuggestions.zsh
[[ -r /usr/share/zsh/plugins/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh ]] && source /usr/share/zsh/plugins/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh
source /usr/share/auxo/shell/prompt.zsh
alias ls='ls --color=auto' ll='ls -lah' grep='grep --color=auto' update='auxo-update'
[[ -o interactive && -z $AUXO_FETCHED ]] && { export AUXO_FETCHED=1; auxo-fetch --small; }
