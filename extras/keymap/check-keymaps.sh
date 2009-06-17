#!/bin/bash

# check that all key names in keymaps/* are known in <linux/input.h>
KEYLIST=./keys.txt
RULES=95-keymap.rules

[ -e "$KEYLIST" ] || {
    echo "need $KEYLIST please build first" >&2
    exit 1
}

missing=$(join -v 2 <(awk '{print tolower(substr($1,5))}' $KEYLIST | sort -u) <(awk '{print $2}' keymaps/*|sort -u))
[ -z "$missing" ] || {
    echo "ERROR: unknown key names in keymaps/*:" >&2
    echo "$missing" >&2
    exit 1
}

# check that all maps referred to in $RULES exist
maps=$(sed -rn '/keymap \$name/ { s/^.*\$name ([^"]+).*$/\1/; p }' $RULES)
for m in $maps; do
    [ -e keymaps/$m ] || {
	echo "ERROR: unknown map name in $RULES: $m" >&2
	exit 1
    }
    grep -q "keymaps/$m\>" Makefile.am || {
	echo "ERROR: map file $m is not added to Makefile.am" >&2
	exit 1
    }
done
