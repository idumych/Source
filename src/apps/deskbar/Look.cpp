/*
 * Copyright 2026 idumych
 * Distributed under the terms of the MIT License.
 */


#include "Look.h"

#include <View.h>


static rgb_color
mix(rgb_color a, rgb_color b, float amount)
{
	rgb_color result;
	result.red = (uint8)(a.red + (b.red - a.red) * amount);
	result.green = (uint8)(a.green + (b.green - a.green) * amount);
	result.blue = (uint8)(a.blue + (b.blue - a.blue) * amount);
	result.alpha = 255;
	return result;
}


desk_look
current_desk_look()
{
	// the theme's own colors: the bar is a menu bar, as the Deskbar's is
	desk_look look;
	look.background = ui_color(B_MENU_BACKGROUND_COLOR);
	look.text = ui_color(B_MENU_ITEM_TEXT_COLOR);
	look.accent = ui_color(B_CONTROL_HIGHLIGHT_COLOR);
	look.dim = mix(look.text, look.background, 0.35f);

	// shades of the background, lighter on a dark theme
	const rgb_color shade = ui_is_dark()
		? make_color(255, 255, 255) : make_color(0, 0, 0);
	look.separator = mix(look.background, shade, 0.12f);
	look.tint = mix(look.background, look.accent, 0.12f);
	look.hover = mix(look.background, look.accent, ui_is_dark() ? 0.3f : 0.2f);
	return look;
}


BFont
desk_font(const char* style, float scale)
{
	BFont font(be_plain_font);
	font_family family;
	font_style plainStyle;
	font.GetFamilyAndStyle(&family, &plainStyle);
	font.SetFamilyAndStyle(family, style);
		// keeps the plain style when the family lacks this one
	font.SetSize(ceilf(font.Size() * scale));
	return font;
}


void
apply_desk_look(BView* view)
{
	const rgb_color background = current_desk_look().background;
	view->SetViewColor(background);
	view->SetLowColor(background);
}


void
draw_desk_highlight(BView* view, BRect frame, bool vertical)
{
	const desk_look look = current_desk_look();
	BRect fill = vertical ? frame.InsetByCopy(4, 1) : frame.InsetByCopy(3, 4);

	view->PushState();
	view->SetDrawingMode(B_OP_OVER);
	view->SetHighColor(look.hover);
	view->FillRoundRect(fill, 6, 6);

	view->SetHighColor(look.accent);
	if (vertical) {
		view->FillRoundRect(BRect(fill.left, fill.top + 4, fill.left + 2,
			fill.bottom - 4), 1, 1);
	} else {
		const float middle = (fill.left + fill.right) / 2;
		view->FillRoundRect(BRect(middle - 8, fill.bottom - 2, middle + 8,
			fill.bottom), 1, 1);
	}
	view->PopState();
}
