#!/bin/sh

# Power Renku starts out in the Renku theme, which needs Tracker to be up for its
# wallpaper, with the Photon shell in the Deskbar's place. Power Theme can go
# back to Haiku's own look with --revert, and to the Deskbar with
# --shell=deskbar.
powertheme=/boot/system/bin/powertheme
[ -x "$powertheme" ] || exit 0

for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	ps | grep -q "^/boot/system/Tracker " && break
	sleep 1
done

"$powertheme" --apply=Renku > /dev/dprintf 2>&1
"$powertheme" --restart-tracker > /dev/dprintf 2>&1
"$powertheme" --shell-every-theme=on --shell=photon > /dev/dprintf 2>&1
