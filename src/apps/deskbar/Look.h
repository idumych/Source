/*
 * Copyright 2026 idumych
 * Distributed under the terms of the MIT License.
 */
#ifndef DESK_LOOK_H
#define DESK_LOOK_H


#include <Font.h>
#include <InterfaceDefs.h>
#include <Rect.h>


class BView;


/*!	The bar's colours, from the running theme's menu colours, with the
	theme's highlight colour as the accent; ui_is_dark() decides which way
	its shades go. Menus that open from the bar keep the theme's look.
*/
struct desk_look {
	rgb_color	background;
	rgb_color	text;
	rgb_color	dim;		// secondary text: dates, counts
	rgb_color	separator;
	rgb_color	hover;
	rgb_color	tint;		// the leaf button
	rgb_color	accent;
};


desk_look	current_desk_look();

// The system font in another weight ("Medium", "SemiBold"), if the family
// has it, and scaled.
BFont		desk_font(const char* style, float scale = 1.0f);

// The view colour and low colour of the bar's own views.
void		apply_desk_look(BView* view);

// An item under the mouse or open: a rounded fill with an accent bar on its
// left edge in a vertical bar, along its bottom in a horizontal one.
void		draw_desk_highlight(BView* view, BRect frame, bool vertical);


#endif	// DESK_LOOK_H
