#!/bin/sh

# Power Renku starts out in its own theme: Power Theme's Renku Light with
# the image's wallpaper, scaled (in ~/config/non-packaged/data/themes),
# or Renku Light itself without it. Power Theme leaves the Deskbar where it
# is, along the bottom. The theme's window
# borders and controls are built once, as a decorator style and a control
# look on create_default_control_look(), so they keep working across
# updates. Power Theme can go back to Haiku's own look with --revert.
powertheme=/boot/system/bin/powertheme
[ -x "$powertheme" ] || exit 0

for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	ps | grep -q "^/boot/system/Tracker " && break
	sleep 1
done

# all but the Deskbar's place; the wallpaper only when asked for
parts=decorator,controls,colors,fonts,icons,wallpaper
"$powertheme" --apply="Power Renku 7" --parts=$parts > /dev/dprintf 2>&1 \
	|| "$powertheme" --apply="Renku Light" --parts=$parts > /dev/dprintf 2>&1
"$powertheme" --restart-tracker > /dev/dprintf 2>&1
