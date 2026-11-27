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
	desk_look look;
	look.accent = ui_color(B_CONTROL_HIGHLIGHT_COLOR);

	if (ui_is_dark()) {
		look.background = make_color(30, 35, 46);
		look.text = make_color(238, 241, 245);
		look.dim = make_color(169, 177, 191);
		look.separator = make_color(54, 59, 69);
		look.hover = mix(look.background, look.accent, 0.3f);
		look.tint = mix(look.background, make_color(255, 255, 255), 0.08f);
	} else {
		look.background = make_color(246, 247, 249);
		look.text = make_color(31, 36, 48);
		look.dim = make_color(91, 98, 112);
		look.separator = make_color(220, 224, 229);
		look.hover = mix(look.background, look.accent, 0.18f);
		look.tint = mix(look.background, look.accent, 0.1f);
	}
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
