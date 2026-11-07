#!/bin/sh

# Power Renku starts out in the Tidy Renku theme, which needs Tracker to be up for its
# wallpaper. Power Theme can go back to Haiku's own look with --revert.
powertheme=/boot/system/bin/powertheme
[ -x "$powertheme" ] || exit 0

for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	ps | grep -q "^/boot/system/Tracker " && break
	sleep 1
done

"$powertheme" --apply="Tidy Renku" > /dev/dprintf 2>&1
"$powertheme" --restart-tracker > /dev/dprintf 2>&1
