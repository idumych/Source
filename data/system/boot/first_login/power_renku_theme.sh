#!/bin/sh

# Power Renku starts out in Power Theme's Expose Air Dark theme, with
# BeOS-style tabs. Power Theme leaves the Deskbar where it is, along the
# bottom. The theme's window borders and controls are built once, as a
# decorator style and a control look on create_default_control_look(), so
# they keep working across updates. Power Theme can go back to Haiku's own
# look with --revert.
powertheme=/boot/system/bin/powertheme
[ -x "$powertheme" ] || exit 0

for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	ps | grep -q "^/boot/system/Tracker " && break
	sleep 1
done

"$powertheme" --apply="Expose Air Dark" > /dev/dprintf 2>&1
"$powertheme" --beos-tabs=on > /dev/dprintf 2>&1
"$powertheme" --restart-tracker > /dev/dprintf 2>&1
