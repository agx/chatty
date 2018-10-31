/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */


#ifndef __ICON_H_INCLUDE__
#define __ICON_H_INCLUDE__

#include "purple.h"

typedef enum
{
   CHATTY_ICON_SIZE_SMALL = 1,
   CHATTY_ICON_SIZE_MEDIUM,
   CHATTY_ICON_SIZE_LARGE
} ChattyPurpleIconSize;

typedef enum
{
   CHATTY_ICON_COLOR_GREY,
   CHATTY_ICON_COLOR_GREEN,
   CHATTY_ICON_COLOR_BLUE
} ChattyPurpleIconColor;


GdkPixbuf *
chatty_icon_get_buddy_icon (PurpleBlistNode *node,
                            guint            scale,
                            guint            color,
                            gboolean         greyed);

void chatty_icon_do_alphashift (GdkPixbuf *pixbuf, int shift);
GtkWidget *chatty_icon_get_avatar_button (int size);

#endif
