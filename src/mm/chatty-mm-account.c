/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* chatty-mm-account.c
 *
 * Copyright 2020, 2021 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "chatty-mm-account"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "chatty-settings.h"
#include "chatty-history.h"
#include "chatty-mm-chat.h"
#include "chatty-utils.h"
#include "itu-e212-iso.h"
#include "chatty-mm-account-private.h"
#include "chatty-mm-account.h"
#include "chatty-log.h"
#include "chatty-mmsd.h"

/**
 * SECTION: chatty-mm-account
 * @title: ChattyMmAccount
 * @short_description: An abstraction over #MMObject
 * @include: "chatty-mm-account.h"
 *
 */

struct _ChattyMmDevice
{
  GObject    parent_instance;

  MMObject  *mm_object;
  gulong     modem_state_id;
};

G_DEFINE_TYPE (ChattyMmDevice, chatty_mm_device, G_TYPE_OBJECT)

static void
chatty_mm_device_finalize (GObject *object)
{
  ChattyMmDevice *self = (ChattyMmDevice *)object;

  g_clear_signal_handler (&self->modem_state_id,
                          mm_object_peek_modem (self->mm_object));
  g_clear_object (&self->mm_object);

  G_OBJECT_CLASS (chatty_mm_device_parent_class)->finalize (object);
}
static void
chatty_mm_device_class_init (ChattyMmDeviceClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = chatty_mm_device_finalize;
}

static void
chatty_mm_device_init (ChattyMmDevice *self)
{
}

static ChattyMmDevice *
chatty_mm_device_new (void)
{
  return g_object_new (CHATTY_TYPE_MM_DEVICE, NULL);
}

MMObject *
chatty_mm_device_get_object (ChattyMmDevice *device)
{
  g_return_val_if_fail (CHATTY_IS_MM_DEVICE (device), NULL);

  return device->mm_object;
}

struct _ChattyMmAccount
{
  ChattyAccount     parent_instance;

  ChattyHistory    *history_db;
  ChattyEds        *chatty_eds;

  MMManager        *mm_manager;
  GListStore       *device_list;
  GListStore       *chat_list;
  GHashTable       *pending_sms;
  GCancellable     *cancellable;

  ChattyStatus      status;

  guint             mm_watch_id;
  gboolean          mm_loaded;
  gboolean          has_mms;

  ChattyMmsd       *mmsd;
};

typedef struct _MessagingData {
  ChattyMmAccount *object;
  char            *message_path;
} MessagingData;

G_DEFINE_TYPE (ChattyMmAccount, chatty_mm_account, CHATTY_TYPE_ACCOUNT)

static int
sort_strv (gconstpointer a,
           gconstpointer b)
{
  char **str_a = (gpointer) a;
  char **str_b = (gpointer) b;

  return g_strcmp0 (*str_a, *str_b);
}

/* numbers:  A comma separated string of numbers */
static char *
create_sorted_numbers (const char *numbers,
                       GPtrArray  *members)
{
  g_autoptr(GPtrArray) sorted = NULL;
  g_autoptr(GString) str = NULL;
  g_auto(GStrv) strv = NULL;
  const char *country_code;

  g_assert (numbers && *numbers);

  strv = g_strsplit (numbers, ",", -1);
  sorted = g_ptr_array_new ();
  country_code = chatty_settings_get_country_iso_code (chatty_settings_get_default ());

  for (guint i = 0; strv[i]; i++) {
    g_autofree char *number = NULL;

    number = chatty_utils_check_phonenumber (strv[i], country_code);
    if (!number)
      number = g_strdup (strv[i]);

    if (members)
      g_ptr_array_add (members, chatty_mm_buddy_new (number, number));
    g_ptr_array_add (sorted, g_strdup (number));
  }

  g_ptr_array_sort (sorted, sort_strv);

  /* Make the array bigger so that we can assure it's NULL terminated */
  g_ptr_array_set_size (sorted, sorted->len + 1);

  for (guint i = 0; i < sorted->len - 1; i++) {
    if (g_strcmp0 (sorted->pdata[i], sorted->pdata[i + 1]) == 0)
      g_ptr_array_remove_index (sorted, i);
  }

  return g_strjoinv (",", (char **)sorted->pdata);
}

static char *
strip_phone_number (const char *number)
{
  g_auto(GStrv) phone = NULL;

  if (!number || !*number)
    return NULL;

  phone = g_strsplit_set (number, "() -", 0);

  return g_strjoinv ("", phone);
}

static ChattyMmDevice *
mm_account_lookup_device (ChattyMmAccount  *self,
                          MMObject         *object,
                          MMModemMessaging *mm_messaging)
{
  guint n_items;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (MM_IS_OBJECT (object) || MM_IS_MODEM_MESSAGING (mm_messaging));

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->device_list));

  for (guint i = 0; i < n_items; i++) {
    g_autoptr(ChattyMmDevice) device = NULL;

    device = g_list_model_get_item (G_LIST_MODEL (self->device_list), i);

    if (device->mm_object == object)
      return g_steal_pointer (&device);

    if (mm_object_peek_modem_messaging (device->mm_object) == mm_messaging)
      return g_steal_pointer (&device);
  }

  return NULL;
}

static void
sent_message_delete_cb (GObject      *object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  ChattyMmAccount *self;
  MMModemMessaging *messaging = (MMModemMessaging *)object;
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  ChattyMessage *message;
  ChattyChat *chat;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  chat = g_object_get_data (G_OBJECT (task), "chat");
  message = g_task_get_task_data (task);
  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (CHATTY_IS_CHAT (chat));

  /* We add the item to db only if we are able to delete it from modem */
  if (mm_modem_messaging_delete_finish (messaging, result, &error))
    chatty_history_add_message (self->history_db, chat, message);
  else if (error)
    g_warning ("Error deleting message: %s", error->message);

  g_task_return_boolean (task, TRUE);
}

static gboolean
get_message_reference (gpointer user_data)
{
  ChattyMmAccount *self;
  GTask *task = user_data;
  ChattyMmDevice *device;
  ChattyMessage *message;
  MMSms *sms;

  g_assert (G_IS_TASK (task));

  sms = g_object_get_data (G_OBJECT (task), "sms");
  self = g_task_get_source_object (task);
  device = g_object_get_data (G_OBJECT (task), "device");
  message = g_task_get_task_data (task);


  chatty_message_set_status (message, CHATTY_STATUS_SENT, 0);
  chatty_message_set_sms_id (message, mm_sms_get_message_reference (sms));
  g_hash_table_insert (self->pending_sms,
                       GINT_TO_POINTER (chatty_message_get_sms_id (message)),
                       g_object_ref (message));

  CHATTY_TRACE_MSG ("deleting message %s", mm_sms_get_path (sms));
  mm_modem_messaging_delete (mm_object_peek_modem_messaging (device->mm_object),
                             mm_sms_get_path (sms),
                             g_task_get_cancellable (task),
                             sent_message_delete_cb,
                             task);

  return G_SOURCE_REMOVE;
}

static void
sms_send_cb (GObject      *object,
             GAsyncResult *result,
             gpointer      user_data)
{
  ChattyMmAccount *self;
  g_autoptr(GTask) task = user_data;
  MMSms *sms = (MMSms *)object;
  ChattyMessage *message;
  ChattyChat *chat;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  chat = g_object_get_data (G_OBJECT (task), "chat");
  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (CHATTY_IS_CHAT (chat));

  message = g_task_get_task_data (task);

  if (!mm_sms_send_finish (sms, result, &error)) {
    chatty_message_set_status (message, CHATTY_STATUS_SENDING_FAILED, 0);
    chatty_history_add_message (self->history_db, chat, message);
    g_debug ("Failed to send sms: %s", error->message);
    g_task_return_error (task, error);
    return;
  }

  chatty_message_set_status (message, CHATTY_STATUS_SENT, 0);

  g_object_set_data_full (G_OBJECT (task), "sms", g_object_ref (sms), g_object_unref);

  /*
   * HACK: There seems some slight delay with updating message_reference with my AT modem.
   * So if mm_sms_get_message_reference (sms) returns 0, try again after some timeout.
   */
  if (mm_sms_get_message_reference (sms))
    get_message_reference (task);
  else
    g_timeout_add (100, get_message_reference, g_steal_pointer (&task));
}

static void
sms_create_cb (GObject      *object,
               GAsyncResult *result,
               gpointer      user_data)
{
  ChattyMmAccount *self;
  g_autoptr(GTask) task = user_data;
  g_autoptr(MMSms) sms = NULL;
  GCancellable *cancellable;
  GError *error = NULL;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CHATTY_IS_MM_ACCOUNT (self));

  sms = mm_modem_messaging_create_finish (MM_MODEM_MESSAGING (object), result, &error);

  if (!sms) {
    ChattyMessage *message;
    ChattyChat *chat;

    chat = g_object_get_data (G_OBJECT (task), "chat");
    message = g_task_get_task_data (task);
    g_assert (CHATTY_IS_CHAT (chat));

    chatty_message_set_status (message, CHATTY_STATUS_SENDING_FAILED, 0);
    chatty_history_add_message (self->history_db, chat, message);

    g_debug ("Failed creating sms: %s", error->message);
    g_task_return_error (task, error);
    return;
  }

  cancellable = g_task_get_cancellable (task);

  CHATTY_TRACE_MSG ("Sending message");
  mm_sms_send (sms, cancellable, sms_send_cb,
               g_steal_pointer (&task));
}

static void
chatty_mm_account_append_message (ChattyMmAccount *self,
                                  ChattyMessage   *message,
                                  ChattyChat      *chat)
{
  guint position;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (CHATTY_IS_MESSAGE (message));
  g_assert (CHATTY_IS_CHAT (chat));

  chatty_mm_chat_append_message (CHATTY_MM_CHAT (chat), message);
  chatty_history_add_message (self->history_db, chat, message);
  g_signal_emit_by_name (chat, "changed", 0);
  if (chatty_message_get_msg_direction (message) == CHATTY_DIRECTION_IN) {
    chatty_chat_show_notification (CHATTY_CHAT (chat),
                                   chatty_item_get_name (CHATTY_ITEM (chat)));
  }

  if (chatty_utils_get_item_position (G_LIST_MODEL (self->chat_list), chat, &position))
    g_list_model_items_changed (G_LIST_MODEL (self->chat_list), position, 1, 1);
}

void
chatty_mm_account_recieve_mms_cb (ChattyMmAccount *self,
                                  ChattyMessage   *message,
                                  const char      *sender,
                                  const char      *recipientlist)
{
  ChattyChat *chat;
  g_autoptr(ChattyMmBuddy) senderbuddy = NULL;
  ChattyMsgDirection message_dir;
  ChattyMessage  *messagecheck;

  chat = chatty_mm_account_start_chat (self, recipientlist);
  /*
   * Check to see if this message exists (e.g. draft MMS sent)
   */

  /*
   *  TODO: I think it would be nicer if I could have a
   *        chatty_mm_chat_find_message_with_uid (), then this will work if
   *        chatty is closed and then reopened.
   */
  messagecheck = chatty_mm_chat_find_message_with_id (CHATTY_MM_CHAT (chat),
                                                      chatty_message_get_id(message));
  if (messagecheck != NULL) {
    chatty_message_set_status (messagecheck, chatty_message_get_status (message), 0);
    chatty_history_add_message (self->history_db, chat, message);
    return;
  }

  message_dir = chatty_message_get_msg_direction (message);
  if (message_dir == CHATTY_DIRECTION_IN) {
    GListModel *users;
    guint items;
    const char *buddy_number;
    g_autofree char *phone1 = NULL;
    g_autofree char *phone2 = NULL;

    /* Find the sender of the message */
    users = chatty_chat_get_users (chat);
    items = g_list_model_get_n_items (users);
    for (guint i = 0; i < items; i++) {
      senderbuddy = g_list_model_get_item (users, i);
      buddy_number = chatty_mm_buddy_get_number (senderbuddy);
      phone1 = chatty_utils_check_phonenumber (buddy_number,
                                               chatty_settings_get_country_iso_code (chatty_settings_get_default ()));
      if (phone1 == NULL) {
        g_warning ("Error with the number!");
        phone1 = g_strdup (buddy_number);
      }
      phone2 = chatty_utils_check_phonenumber (sender,
                                               chatty_settings_get_country_iso_code (chatty_settings_get_default ()));
      if (phone2 == NULL) {
        phone2 = g_strdup (buddy_number);
      }
      if (g_strcmp0 (phone1, phone2) == 0) {
        break;
      }
    }
  } else if (message_dir == CHATTY_DIRECTION_OUT) {
    senderbuddy = chatty_mm_buddy_new (sender, sender);
  }
  chatty_message_set_user (message, CHATTY_ITEM (senderbuddy));

  chatty_mm_account_append_message (self, message, chat);
}

static void
mm_account_delete_message_async (ChattyMmAccount     *self,
                                 ChattyMmDevice      *device,
                                 MMSms               *sms,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  const char *sms_path;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (CHATTY_IS_MM_DEVICE (device));
  g_assert (MM_IS_SMS (sms));

  sms_path = mm_sms_get_path (sms);
  CHATTY_TRACE_MSG ("deleting message %s", sms_path);
  mm_modem_messaging_delete (mm_object_peek_modem_messaging (device->mm_object),
                             sms_path, self->cancellable, NULL, NULL);
}

static gboolean
mm_account_add_sms (ChattyMmAccount *self,
                    ChattyMmDevice  *device,
                    MMSms           *sms,
                    MMSmsState       state)
{
  g_autoptr(ChattyMessage) message = NULL;
  g_autoptr(GDateTime) date_time = NULL;
  g_autoptr(ChattyMmBuddy) senderbuddy = NULL;
  ChattyChat *chat;
  g_autofree char *phone = NULL;
  g_autofree char *uuid = NULL;
  const char *msg;
  ChattyMsgDirection direction = CHATTY_DIRECTION_UNKNOWN;
  gint64 unix_time = 0;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (MM_IS_SMS (sms));

  msg = mm_sms_get_text (sms);
  if (!msg)
    return FALSE;

  phone = chatty_utils_check_phonenumber (mm_sms_get_number (sms),
                                          chatty_settings_get_country_iso_code (chatty_settings_get_default ()));
  if (!phone)
    phone = mm_sms_dup_number (sms);

  CHATTY_TRACE (phone, "received message from ");

  chat = chatty_mm_account_start_chat (self, phone);

  if (state == MM_SMS_STATE_RECEIVED) {
    direction = CHATTY_DIRECTION_IN;
    senderbuddy = chatty_mm_chat_find_user (CHATTY_MM_CHAT (chat), phone);
    if (senderbuddy)
      g_object_ref (senderbuddy);
  } else if (state == MM_SMS_STATE_SENT) {
    direction = CHATTY_DIRECTION_OUT;
    senderbuddy = chatty_mm_buddy_new (phone, phone);
  }

  date_time = g_date_time_new_from_iso8601 (mm_sms_get_timestamp (sms), NULL);
  if (date_time)
    unix_time = g_date_time_to_unix (date_time);
  if (!unix_time)
    unix_time = time (NULL);

  uuid = g_uuid_string_random ();
  message = chatty_message_new (CHATTY_ITEM (senderbuddy),
                                msg, uuid, unix_time, CHATTY_MESSAGE_TEXT, direction, 0);

  chatty_mm_account_append_message (self, message, chat);

  if (direction == CHATTY_DIRECTION_IN)
    mm_account_delete_message_async (self, device, sms, NULL, NULL);

  return TRUE;
}

static void
sms_state_changed_cb (ChattyMmAccount *self,
                      GParamSpec      *pspec,
                      MMSms           *sms)
{
  MMSmsState state;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (MM_IS_SMS (sms));

  state = mm_sms_get_state (sms);

  if (state == MM_SMS_STATE_RECEIVED) {
    ChattyMmDevice *device;

    device = g_object_get_data (G_OBJECT (sms), "device");
    if (mm_account_add_sms (self, device, sms, state)) {
      CHATTY_TRACE_MSG ("deleting message %s", mm_sms_get_path (sms));
      mm_modem_messaging_delete (mm_object_peek_modem_messaging (device->mm_object),
                                 mm_sms_get_path (sms),
                                 NULL, NULL, NULL);
    }
  }
}

static void
parse_sms (ChattyMmAccount *self,
           ChattyMmDevice  *device,
           MMSms           *sms)
{
  MMSmsPduType type;
  MMSmsState state;
  guint sms_id;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (MM_IS_SMS (sms));

  sms_id = mm_sms_get_message_reference (sms);
  g_debug ("parsing sms, id: %u, path: %s", sms_id, mm_sms_get_path (sms));
  state = mm_sms_get_state (sms);
  type = mm_sms_get_pdu_type (sms);

  if (state == MM_SMS_STATE_SENDING ||
      state == MM_SMS_STATE_SENT)
    return;

  if (type == MM_SMS_PDU_TYPE_STATUS_REPORT) {
    guint delivery_state;

    delivery_state = mm_sms_get_delivery_state (sms);
    if (delivery_state <= MM_SMS_DELIVERY_STATE_COMPLETED_REPLACED_BY_SC) {
      ChattyMessage *message;

      message = g_hash_table_lookup (self->pending_sms, GINT_TO_POINTER (sms_id));
      if (message) {
        ChattyChat *chat;

        chatty_message_set_status (message, CHATTY_STATUS_DELIVERED, 0);
        chat = chatty_mm_account_find_chat (self, mm_sms_get_number (sms));
        if (chat)
          chatty_history_add_message (self->history_db, chat, message);
      }

      CHATTY_TRACE_MSG ("deleting message %s", mm_sms_get_path (sms));
      mm_modem_messaging_delete (mm_object_peek_modem_messaging (device->mm_object),
                                 mm_sms_get_path (sms),
                                 NULL, NULL, NULL);
      g_hash_table_remove (self->pending_sms, GINT_TO_POINTER (sms_id));
    }
  } else if (type == MM_SMS_PDU_TYPE_CDMA_DELIVER ||
             type == MM_SMS_PDU_TYPE_DELIVER) {
    if (state == MM_SMS_STATE_RECEIVED && mm_account_add_sms (self, device, sms, state)) {
        CHATTY_TRACE_MSG ("deleting message %s", mm_sms_get_path (sms));
        mm_modem_messaging_delete (mm_object_peek_modem_messaging (device->mm_object),
                                   mm_sms_get_path (sms),
                                   NULL, NULL, NULL);
    } else if (state == MM_SMS_STATE_RECEIVING) {
      g_object_set_data_full (G_OBJECT (sms), "device",
                              g_object_ref (device),
                              g_object_unref);
      g_signal_connect_object (sms, "notify::state",
                               G_CALLBACK (sms_state_changed_cb),
                               self,
                               G_CONNECT_SWAPPED | G_CONNECT_AFTER);
    }
  }
}

static void
mm_account_messaging_list_cb (GObject      *object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  ChattyMmAccount *self;
  MMModemMessaging *mm_messaging = (MMModemMessaging *)object;
  MessagingData *data = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(ChattyMmDevice) device = NULL;
  GList *list;
  char *path;

  g_assert (data);
  g_assert (CHATTY_IS_MM_ACCOUNT (data->object));
  self = data->object;

  list = mm_modem_messaging_list_finish (mm_messaging, result, &error);
  CHATTY_TRACE_MSG ("message listed. has-error: %d, message count: %d",
                    !!error, g_list_length (list));

  if (error) {
    g_debug ("Error listing messages: %s", error->message);
    return;
  }

  path = data->message_path;
  device = mm_account_lookup_device (self, NULL, mm_messaging);

  for (GList *node = list; node; node = node->next)
    if (!path || g_str_equal (mm_sms_get_path (node->data), path)) {
      parse_sms (self, device, node->data);

      if (path)
        break;
    }

  g_object_unref (data->object);
  g_free (data->message_path);
  g_free (data);
}

static void
mm_account_sms_recieved_cb (ChattyMmAccount  *self,
                            char             *arg_path,
                            gboolean          arg_received,
                            MMModemMessaging *mm_messaging)
{
  MessagingData *data;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (MM_IS_MODEM_MESSAGING (mm_messaging));

  if (!arg_path)
    return;

  data = g_new0 (MessagingData, 1);
  data->object = g_object_ref (self);
  data->message_path = g_strdup (arg_path);

  CHATTY_TRACE_MSG ("List modem messages");

  mm_modem_messaging_list (mm_messaging, self->cancellable,
                           mm_account_messaging_list_cb,
                           data);
}

static void
mm_account_modem_state_changed (ChattyMmAccount *self,
                                GParamSpec      *pspec,
                                MMModem         *modem)
{
  MMModemState state;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));

  state = mm_modem_get_state (modem);

  if ((state <= MM_MODEM_STATE_ENABLING &&
      self->status == CHATTY_CONNECTED) ||
      (state > MM_MODEM_STATE_ENABLING &&
       self->status != CHATTY_CONNECTED)) {
    self->status = CHATTY_UNKNOWN;
    g_object_notify (G_OBJECT (self), "status");
  }
}

static void
mm_object_added_cb (ChattyMmAccount *self,
                    GDBusObject     *object)
{
  g_autoptr(ChattyMmAccount) account = NULL;
  g_autoptr(ChattyMmDevice) device = NULL;
  GListModel *chat_list;
  MessagingData *data;
  MMSim *sim;
  guint n_chats;
  ChattySettings *settings;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (MM_IS_OBJECT (object));

  CHATTY_TRACE_MSG ("modem %p found, has messaging: %d", object,
                    !!mm_object_peek_modem_messaging (MM_OBJECT (object)));

  if (!mm_object_peek_modem_messaging (MM_OBJECT (object)))
    return;

  settings = chatty_settings_get_default ();
  device = chatty_mm_device_new ();
  device->mm_object = g_object_ref (MM_OBJECT (object));

  device->modem_state_id = g_signal_connect_swapped (mm_object_peek_modem (device->mm_object),
                                                     "notify::state",
                                                     G_CALLBACK (mm_account_modem_state_changed),
                                                     self);
  g_list_store_append (self->device_list, device);

  if (self->status != CHATTY_CONNECTED) {
    self->status = CHATTY_UNKNOWN;
    g_object_notify (G_OBJECT (self), "status");
  }

  /* We already have the messaging object, so SIM should be ready too */
  sim = mm_modem_get_sim_sync (mm_object_peek_modem (MM_OBJECT (object)),
                               NULL, NULL);

  if (sim) {
    const char *code;

    code = get_country_iso_for_mcc (mm_sim_get_imsi (sim));

    if (code && *code)
      chatty_settings_set_country_iso_code (settings, code);
  }

  chat_list = chatty_mm_account_get_chat_list (self);
  n_chats = g_list_model_get_n_items (chat_list);
  for (guint i = 0; i < n_chats; i++) {
    g_autoptr(ChattyMmChat) chat = NULL;

    chat = g_list_model_get_item (chat_list, i);
    chatty_mm_chat_refresh (chat);
  }

  g_signal_connect_swapped (mm_object_peek_modem_messaging (MM_OBJECT (object)),
                            "added",
                            G_CALLBACK (mm_account_sms_recieved_cb), self);
  CHATTY_TRACE_MSG ("List messages from modem %p", object);

  data = g_new0 (MessagingData, 1);
  data->object = g_object_ref (self);

  mm_modem_messaging_list (mm_object_peek_modem_messaging (MM_OBJECT (object)),
                           NULL,
                           mm_account_messaging_list_cb,
                           data);
}

static void
mm_object_removed_cb (ChattyMmAccount *self,
                      GDBusObject     *object)
{
  gsize n_items;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (MM_IS_OBJECT (object));

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->device_list));

  for (guint i = 0; i < n_items; i++) {
    g_autoptr(ChattyMmDevice) device = NULL;

    device = g_list_model_get_item (G_LIST_MODEL (self->device_list), i);
    if (g_strcmp0 (mm_object_get_path (MM_OBJECT (object)),
                   mm_object_get_path (device->mm_object)) == 0) {
      self->status = CHATTY_UNKNOWN;
      g_list_store_remove (self->device_list, i);
      break;
    }
  }

  if (self->status == CHATTY_UNKNOWN)
    g_object_notify (G_OBJECT (self), "status");
}

static void
mm_interface_added_cb (ChattyMmAccount *self,
                       GDBusObject     *object,
                       GDBusInterface  *interface)
{
  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (G_IS_DBUS_INTERFACE (interface));

  if (MM_IS_MODEM_MESSAGING (interface))
    mm_object_added_cb (self, object);
}

static void
mm_interface_removed_cb (ChattyMmAccount *self,
                         GDBusObject     *object,
                         GDBusInterface  *interface)
{
  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (G_IS_DBUS_INTERFACE (interface));

  if (MM_IS_MODEM_MESSAGING (interface))
    mm_object_removed_cb (self, object);
}

static void mm_new_cb (GObject      *object,
                       GAsyncResult *result,
                       gpointer      user_data);
static void
mm_appeared_cb (GDBusConnection *connection,
                const char      *name,
                const char      *name_owner,
                ChattyMmAccount *self)
{
  GTask *task;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (G_IS_DBUS_CONNECTION (connection));

  g_debug ("Modem Manager appeared");

  if (self->mm_manager)
    return;

  task = g_task_new (self, NULL, NULL, NULL);
  mm_manager_new (connection,
                  G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                  NULL, mm_new_cb, task);
}

static void
mm_vanished_cb (GDBusConnection *connection,
                const char      *name,
                ChattyMmAccount *self)
{
  g_assert (CHATTY_IS_MM_ACCOUNT (self));
  g_assert (G_IS_DBUS_CONNECTION (connection));

  g_debug ("Modem Manager vanished");

  g_clear_object (&self->mm_manager);
  g_list_store_remove_all (self->device_list);

  self->status = CHATTY_UNKNOWN;
  g_object_notify (G_OBJECT (self), "status");
}

static void
mm_new_cb (GObject      *object,
           GAsyncResult *result,
           gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  ChattyMmAccount *self;
  GError *error = NULL;
  GList *objects;

  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  g_assert (CHATTY_IS_MM_ACCOUNT (self));

  self->mm_manager = mm_manager_new_finish (result, &error);
  if (chatty_settings_get_experimental_features (chatty_settings_get_default ())) {
    /* Load chatty_mmsd */
    chatty_mmsd_load (self->mmsd);
  }

  if (!self->mm_watch_id)
    self->mm_watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                                          MM_DBUS_SERVICE,
                                          G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                          (GBusNameAppearedCallback)mm_appeared_cb,
                                          (GBusNameVanishedCallback)mm_vanished_cb,
                                          g_object_ref (self),
                                          g_object_unref);
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Error creating ModemManager: %s", error->message);
    g_task_return_error (task, error);
    return;
  }

  g_signal_connect_swapped (G_DBUS_OBJECT_MANAGER (self->mm_manager),
                            "object-added",
                            G_CALLBACK (mm_object_added_cb), self);
  g_signal_connect_swapped (G_DBUS_OBJECT_MANAGER (self->mm_manager),
                            "object-removed",
                            G_CALLBACK (mm_object_removed_cb), self);
  g_signal_connect_swapped (G_DBUS_OBJECT_MANAGER (self->mm_manager),
                            "interface-added",
                            G_CALLBACK (mm_interface_added_cb), self);
  g_signal_connect_swapped (G_DBUS_OBJECT_MANAGER (self->mm_manager),
                            "interface-removed",
                            G_CALLBACK (mm_interface_removed_cb), self);

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (self->mm_manager));

  for (GList *node = objects; node; node = node->next)
    mm_object_added_cb (self, node->data);
  g_list_free_full (objects, g_object_unref);

  g_task_return_boolean (task, TRUE);
}

static const char *
chatty_mm_account_get_protocol_name (ChattyAccount *account)
{
  return "SMS";
}

static ChattyStatus
chatty_mm_account_get_status (ChattyAccount *account)
{
  ChattyMmAccount *self = (ChattyMmAccount *)account;
  GListModel *devices;
  guint n_items;

  g_assert (CHATTY_IS_MM_ACCOUNT (self));

  if (self->status != CHATTY_UNKNOWN)
    return self->status;

  devices = G_LIST_MODEL (self->device_list);
  n_items = g_list_model_get_n_items (devices);

  if (n_items == 0)
    self->status = CHATTY_DISCONNECTED;

  for (guint i = 0; i < n_items; i++) {
    g_autoptr(ChattyMmDevice) device = NULL;
    MMModem *modem;

    device = g_list_model_get_item (devices, i);
    modem = mm_object_peek_modem (device->mm_object);

    if (modem && mm_modem_get_state (modem) >= MM_MODEM_STATE_ENABLED) {
      self->status = CHATTY_CONNECTED;
      break;
    }
  }

  if (self->status == CHATTY_UNKNOWN)
    self->status = CHATTY_DISCONNECTED;

  return self->status;
}

static ChattyProtocol
chatty_mm_account_get_protocols (ChattyItem *item)
{
  return CHATTY_PROTOCOL_MMS_SMS;
}

static const char *
chatty_mm_account_get_username (ChattyItem *item)
{
  return "SMS";
}

static void
chatty_mm_account_finalize (GObject *object)
{
  ChattyMmAccount *self = (ChattyMmAccount *)object;

  if (self->cancellable)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_list_store_remove_all (self->device_list);

  g_clear_handle_id (&self->mm_watch_id, g_bus_unwatch_name);
  g_clear_object (&self->history_db);
  g_clear_object (&self->chat_list);
  g_clear_object (&self->device_list);
  g_clear_object (&self->chatty_eds);
  g_clear_object (&self->mmsd);
  g_hash_table_unref (self->pending_sms);

  G_OBJECT_CLASS (chatty_mm_account_parent_class)->finalize (object);
}

static void
chatty_mm_account_class_init (ChattyMmAccountClass *klass)
{
  GObjectClass *object_class  = G_OBJECT_CLASS (klass);
  ChattyItemClass *item_class = CHATTY_ITEM_CLASS (klass);
  ChattyAccountClass *account_class = CHATTY_ACCOUNT_CLASS (klass);

  object_class->finalize = chatty_mm_account_finalize;

  item_class->get_protocols = chatty_mm_account_get_protocols;
  item_class->get_username = chatty_mm_account_get_username;

  account_class->get_protocol_name = chatty_mm_account_get_protocol_name;
  account_class->get_status   = chatty_mm_account_get_status;
}

static void
chatty_mm_account_init (ChattyMmAccount *self)
{
  self->chat_list = g_list_store_new (CHATTY_TYPE_MM_CHAT);
  self->device_list = g_list_store_new (CHATTY_TYPE_MM_DEVICE);
  self->mmsd = chatty_mmsd_new (self);
  self->pending_sms = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                             NULL, g_object_unref);
  self->has_mms = FALSE;
}

ChattyMmAccount *
chatty_mm_account_new (void)
{
  return g_object_new (CHATTY_TYPE_MM_ACCOUNT, NULL);
}

void
chatty_mm_account_set_eds (ChattyMmAccount *self,
                           ChattyEds       *eds)
{
  guint n_items;

  g_return_if_fail (CHATTY_IS_MM_ACCOUNT (self));
  g_return_if_fail (!eds || CHATTY_IS_EDS (eds));

  if (!g_set_object (&self->chatty_eds, eds))
    return;

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->chat_list));
  for (guint i = 0; i < n_items; i++) {
    g_autoptr(ChattyMmChat) chat = NULL;

    chat = g_list_model_get_item (G_LIST_MODEL (self->chat_list), i);
    chatty_mm_chat_set_eds (chat, self->chatty_eds);
  }
}

void
chatty_mm_account_set_history_db (ChattyMmAccount *self,
                                  gpointer         history_db)
{
  g_return_if_fail (CHATTY_IS_MM_ACCOUNT (self));
  g_return_if_fail (!history_db || CHATTY_IS_HISTORY (history_db));
  g_return_if_fail (!self->history_db);

  g_set_object (&self->history_db, history_db);
}

GListModel *
chatty_mm_account_get_chat_list (ChattyMmAccount *self)
{
  g_return_val_if_fail (CHATTY_IS_MM_ACCOUNT (self), NULL);

  return G_LIST_MODEL (self->chat_list);
}

static void
get_bus_cb (GObject      *object,
            GAsyncResult *result,
            gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(GDBusConnection) connection = NULL;
  GCancellable *cancellable;
  GError *error = NULL;

  connection = g_bus_get_finish (result, &error);

  if (error) {
    g_warning ("Error getting bus: %s", error->message);
    g_task_return_error (task, error);
    return;
  }

  cancellable = g_task_get_cancellable (task);
  mm_manager_new (connection,
                  G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                  cancellable, mm_new_cb,
                  g_steal_pointer (&task));
}

static void
mm_get_chats_cb (GObject      *object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  ChattyMmAccount *self;
  GTask *task = user_data;
  GPtrArray *chats = NULL;
  GCancellable *cancellable;
  g_autoptr(GError) error = NULL;

  self = g_task_get_source_object (task);
  g_assert (CHATTY_IS_MM_ACCOUNT (self));

  chats = chatty_history_get_chats_finish (self->history_db, result, &error);

  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("Error loading chat: %s", error->message);

  CHATTY_TRACE_MSG ("Loaded %d chats from history db", !chats ? 0 : chats->len);

  if (chats) {
    for (guint i = 0; i < chats->len; i++) {
      chatty_chat_set_data (chats->pdata[i], self, self->history_db);
      chatty_mm_chat_set_eds (chats->pdata[i], self->chatty_eds);
    }

    g_list_store_splice (self->chat_list, 0, 0, chats->pdata, chats->len);
  }

  cancellable = g_task_get_cancellable (task);
  g_bus_get (G_BUS_TYPE_SYSTEM, cancellable, get_bus_cb, task);
}

void
chatty_mm_account_load_async (ChattyMmAccount     *self,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (CHATTY_IS_MM_ACCOUNT (self));
  g_return_if_fail (self->history_db);
  g_return_if_fail (!self->mm_watch_id);
  g_return_if_fail (!self->mm_loaded);

  if (!self->cancellable)
    self->cancellable = g_cancellable_new ();

  self->mm_loaded = TRUE;
  task = g_task_new (self, self->cancellable, callback, user_data);
  CHATTY_TRACE_MSG ("Loading chats from history db");
  chatty_history_get_chats_async (self->history_db, CHATTY_ACCOUNT (self),
                                  mm_get_chats_cb, task);
}

gboolean
chatty_mm_account_load_finish (ChattyMmAccount  *self,
                               GAsyncResult     *result,
                               GError          **error)
{
  g_return_val_if_fail (CHATTY_MM_ACCOUNT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

ChattyChat *
chatty_mm_account_find_chat (ChattyMmAccount *self,
                             const char      *recipientlist)
{
  g_autofree char *sorted_name = NULL;
  gulong n_items;

  g_return_val_if_fail (CHATTY_MM_ACCOUNT (self), NULL);

  g_assert (CHATTY_IS_MM_ACCOUNT (self));

  if (!recipientlist || !*recipientlist)
    return NULL;

  /*
   * chatty-mmsd will return a comma seperated list of
   * the sender and recipients. chatty-mmsd will remove the modem number,
   * so chatty_mm_account_find_chat () does not have to account for that.
   * If there is only one recipient, then this will behave the exact
   * same way as the old chatty_mm_account_find_chat ()
   */
  sorted_name = create_sorted_numbers (recipientlist, NULL);
  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->chat_list));

  for (guint i = 0; i < n_items; i++) {
    g_autoptr (ChattyChat) chat = NULL;

    chat = g_list_model_get_item (G_LIST_MODEL (self->chat_list), i);

    if (g_strcmp0 (chatty_chat_get_chat_name (chat), sorted_name) == 0)
      return chat;
  }

  return NULL;
}

ChattyChat *
chatty_mm_account_start_chat (ChattyMmAccount *self,
                              const char      *recipientlist)
{
  ChattyChat *chat;

  g_return_val_if_fail (CHATTY_IS_MM_ACCOUNT (self), NULL);

  if (!recipientlist || !*recipientlist)
    return NULL;

  chat = chatty_mm_account_find_chat (self, recipientlist);
  if (!chat) {
    g_autoptr(GPtrArray) members = NULL;
    g_autofree char *sorted_name = NULL;

    members = g_ptr_array_new_full (1, g_object_unref);
    sorted_name = create_sorted_numbers (recipientlist, members);

    if (members->len == 1)
      chat = (ChattyChat *)chatty_mm_chat_new (sorted_name, NULL, CHATTY_PROTOCOL_MMS_SMS, TRUE);
    else /* Only MMS has multiple recipients */
      chat = (ChattyChat *)chatty_mm_chat_new (sorted_name, NULL, CHATTY_PROTOCOL_MMS, FALSE);

    chatty_mm_chat_add_users (CHATTY_MM_CHAT (chat), members);
    chatty_chat_set_data (chat, self, self->history_db);
    chatty_mm_chat_set_eds (CHATTY_MM_CHAT (chat), self->chatty_eds);

    g_list_store_append (self->chat_list, chat);
    g_object_unref (chat);
  }

  return chat;
}

void
chatty_mm_account_delete_chat (ChattyMmAccount *self,
                               ChattyChat      *chat)
{
  g_return_if_fail (CHATTY_IS_MM_ACCOUNT (self));
  g_return_if_fail (CHATTY_IS_MM_CHAT (chat));

  chatty_utils_remove_list_item (self->chat_list, chat);
}

gboolean
chatty_mm_account_has_mms_feature (ChattyMmAccount *self)
{
  g_return_val_if_fail (CHATTY_IS_MM_ACCOUNT (self), FALSE);

  if (self->mmsd)
    return chatty_mmsd_is_ready (self->mmsd);

  return FALSE;
}

void
chatty_mm_account_send_message_async (ChattyMmAccount     *self,
                                      ChattyChat          *chat,
                                      ChattyMmBuddy       *buddy,
                                      ChattyMessage       *message,
                                      gboolean             is_mms,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
  g_autoptr(MMSmsProperties) sms_properties = NULL;
  g_autoptr(GTask) task = NULL;
  g_autofree char *phone = NULL;
  ChattySettings *settings;
  ChattyMmDevice *device;
  guint position;
  gboolean request_report;

  g_return_if_fail (CHATTY_IS_MM_ACCOUNT (self));
  g_return_if_fail (CHATTY_IS_MM_CHAT (chat));
  if (!is_mms)
    g_return_if_fail (CHATTY_IS_MM_BUDDY (buddy));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, message, NULL);

  g_object_set_data_full (G_OBJECT (task), "chat", g_object_ref (chat),
                          g_object_unref);

  device = g_list_model_get_item (G_LIST_MODEL (self->device_list), 0);
  g_object_set_data_full (G_OBJECT (task), "device", device, g_object_unref);

  if (!device) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "No modem found");
    return;
  }

  if (is_mms) {
    CHATTY_TRACE_MSG ("Creating MMS message");

    chatty_mmsd_send_mms_async (self->mmsd, chat, message, g_steal_pointer (&task));
    return;
  }

  phone = strip_phone_number (chatty_mm_buddy_get_number (buddy));

  if (!phone || !*phone) {
    g_task_return_new_error (task,
                             G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "%s is not a valid phone number",
                             chatty_mm_buddy_get_number (buddy));
    return;
  }

  settings = chatty_settings_get_default ();
  request_report = chatty_settings_request_sms_delivery_reports (settings);
  sms_properties = mm_sms_properties_new ();
  mm_sms_properties_set_text (sms_properties, chatty_message_get_text (message));
  mm_sms_properties_set_number (sms_properties, phone);
  mm_sms_properties_set_delivery_report_request (sms_properties, request_report);
  mm_sms_properties_set_validity_relative (sms_properties, 168);

  if (chatty_utils_get_item_position (G_LIST_MODEL (self->chat_list), chat, &position))
    g_list_model_items_changed (G_LIST_MODEL (self->chat_list), position, 1, 1);

  CHATTY_TRACE (phone, "Creating sms message to number: ");
  mm_modem_messaging_create (mm_object_peek_modem_messaging (device->mm_object),
                             sms_properties, cancellable,
                             sms_create_cb,
                             g_steal_pointer (&task));
}

gboolean
chatty_mm_account_send_message_finish (ChattyMmAccount *self,
                                       GAsyncResult    *result,
                                       GError          **error)
{
  g_return_val_if_fail (CHATTY_MM_ACCOUNT (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

GListModel *
chatty_mm_account_get_devices (ChattyMmAccount *self)
{
  g_return_val_if_fail (CHATTY_MM_ACCOUNT (self), NULL);

  return G_LIST_MODEL (self->device_list);
}