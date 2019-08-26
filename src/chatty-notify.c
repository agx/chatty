/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "chatty-notify"

#include <glib.h>
#include <glib/gi18n.h>
#include "purple.h"
#include "chatty-window.h"
#include "chatty-notify.h"


static void
cb_open_message (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  // TODO: switch to the conversation view
}


static void
cb_open_settings (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  chatty_window_change_view (CHATTY_VIEW_SETTINGS);
}


static const GActionEntry actions[] = {
  { "open-message", cb_open_message, "s" },
  { "open-settings", cb_open_settings }
};


void
chatty_notify_show_notification (const char *message,
                                 guint       notification_type,
                                 const char *buddy_name)
{
  GApplication  *application;
  GNotification *notification;
  GIcon         *icon;

  if (!message) {
    return;
  }

  application = g_application_get_default ();

  notification = g_notification_new (_("Chatty"));

  g_notification_set_body (notification, message);

  icon = g_icon_new_for_string ("sm.puri.Chatty-symbolic", NULL);

  if (icon) {
    g_notification_set_icon (notification, icon);
  }

  switch (notification_type) {
    case CHATTY_NOTIFY_TYPE_MESSAGE:
      g_notification_add_button_with_target (notification,
                                             _("Open Message"),
                                             "app.open-message",
                                             "s",
                                             buddy_name);

      g_notification_set_priority (notification, G_NOTIFICATION_PRIORITY_LOW);
      g_application_send_notification (application, "conversation-message", notification);
      break;

    case CHATTY_NOTIFY_TYPE_ACCOUNT:
      g_notification_add_button (notification,
                                 _("Open Account Settings"),
                                 "app.open-settings");

      g_notification_set_priority (notification, G_NOTIFICATION_PRIORITY_HIGH);
      g_application_send_notification (application, "account-message", notification);
      break;

    case CHATTY_NOTIFY_TYPE_GENERIC:
      g_notification_set_priority (notification, G_NOTIFICATION_PRIORITY_NORMAL);
      g_application_send_notification (application, "generic-message", notification);
      break;

    case CHATTY_NOTIFY_TYPE_ERROR:
      g_notification_set_priority (notification, G_NOTIFICATION_PRIORITY_URGENT);
      g_application_send_notification (application, "error-message", notification);
      break;

    default:
      g_notification_set_priority (notification, G_NOTIFICATION_PRIORITY_LOW);
      g_application_send_notification (application, "undefined-message", notification);
  }

  g_action_map_add_action_entries (G_ACTION_MAP (application),
                                   actions,
                                   G_N_ELEMENTS (actions),
                                   application);

  g_object_unref (notification);
}
