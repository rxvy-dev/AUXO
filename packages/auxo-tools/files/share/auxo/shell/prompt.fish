# Auxo fish prompt
function fish_prompt
    set -l st $status
    set -l a (string match -r 'ACCENT="?([a-z]+)' < /etc/auxo/auxo.conf 2>/dev/null)[2]
    set -l c a78bfa
    switch "$a"
        case cyan; set c 22d3ee
        case emerald; set c 34d399
        case amber; set c fbbf24
        case rose; set c fb7185
        case blue; set c 60a5fa
        case mono; set c e5e7eb
    end
    set_color $c; echo -n '▲ '; set_color --bold normal; echo -n (prompt_pwd); set_color normal
    echo -n (fish_git_prompt ' on %s')
    test $st -eq 0; and set_color $c; or set_color red
    echo -n ' ❯ '; set_color normal
end

# Auxo greeting in each new terminal window (login shells already showed /etc/motd)
if status is-interactive; and not status is-login; and not set -q AUXO_MOTD_SHOWN; and command -q auxo-motd
    set -gx AUXO_MOTD_SHOWN 1
    auxo-motd
end
