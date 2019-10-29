/*
 * Copyright (C) 2018 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "chatty-folks"

#include <glib.h>
#include <gee-0.8/gee.h>
#include <folks/folks.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "purple.h"
#include "chatty-utils.h"
#include "chatty-folks.h"
#include "chatty-window.h"
#include "chatty-contact-row.h"
#include "chatty-utils.h"


static chatty_folks_data_t chatty_folks_data;
static GdkPixbuf *chatty_folks_shape_pixbuf (GdkPixbuf *pixbuf);
static void chatty_folks_individual_add_contact_rows (FolksIndividual *individual);

chatty_folks_data_t *chatty_get_folks_data (void)
{
  return &chatty_folks_data;
}


typedef struct {
  FolksIndividual  *individual;
  ChattyContactRow *row;
  GInputStream     *stream;
  PurpleAccount    *purple_account;
  const char       *purple_user_name;
  int               mode;
  int               size;
} AvatarData;


static void
cb_aggregator_prepare_finish (FolksIndividualAggregator *aggregator,
                              GAsyncResult              *res,
                              gpointer                   user_data)
{
  folks_individual_aggregator_prepare_finish (aggregator, res, NULL);
}


static void
cb_update_row (FolksIndividual *individual,
               GParamSpec      *pspec,
               gpointer         user_data)
{
  GList      *rows, *l;
  const char *id;
  const char *row_id;

  chatty_folks_data_t *chatty_folks = chatty_get_folks_data ();

  // since a contact-row is created for each phone-number of 
  // an individual, and there may well be another approach UI wise,
  // we just recreate the related rows for the time being instead 
  // of updating them

  id = folks_individual_get_id (individual);    

  rows = gtk_container_get_children (GTK_CONTAINER(chatty_folks->listbox));
   
  if (gee_map_get (chatty_folks->individuals, id)) {
    for (l = rows; l; l = l->next) {
      if (l->data != NULL) {
        g_object_get (l->data, "id", &row_id, NULL);

        if (!g_strcmp0 (id, row_id )) {
          gtk_widget_destroy (GTK_WIDGET(l->data));
        }
      }
    }

    chatty_folks_individual_add_contact_rows (individual);

    gtk_list_box_invalidate_sort (chatty_folks->listbox);

    g_list_free (l);
    g_list_free (rows);
  }
}


static void
cb_aggregator_notify (FolksIndividualAggregator *aggregator,
                      GParamSpec                *pspec,
                      gpointer                   user_data)
{
  GeeMapIterator  *iter;
  FolksIndividual *individual;

  chatty_folks_data_t *chatty_folks = chatty_get_folks_data ();

  chatty_folks->individuals = folks_individual_aggregator_get_individuals (aggregator);
  iter = gee_map_map_iterator (chatty_folks->individuals);

  while (gee_map_iterator_next (iter)) {
    individual = gee_map_iterator_get_value (iter);

    g_signal_connect (G_OBJECT(individual), 
                      "notify::avatar",
                      G_CALLBACK (cb_update_row),
                      NULL);

    g_signal_connect (G_OBJECT(individual), 
                      "notify::display-name",
                      G_CALLBACK (cb_update_row),
                      NULL);

    g_signal_connect (G_OBJECT(individual), 
                      "notify::phone-numbers",
                      G_CALLBACK (cb_update_row),
                      NULL);
  }

  g_clear_object (&iter);
}


static void
cb_aggregator_individuals_changed (FolksIndividualAggregator *aggregator,
                                   GeeMultiMap               *changes,
                                   gpointer                   user_data)
{
  GeeIterator   *iter;
  GeeSet        *removed;
  GeeCollection *added;
  GList         *rows, *l;
  const char    *id;
  char          *row_id;


  chatty_folks_data_t *chatty_folks = chatty_get_folks_data ();

  removed = gee_multi_map_get_keys (changes);
  added = gee_multi_map_get_values (changes);

  chatty_folks->individuals = folks_individual_aggregator_get_individuals (aggregator);

  iter = gee_iterable_iterator (GEE_ITERABLE (removed));

  rows = gtk_container_get_children (GTK_CONTAINER(chatty_folks->listbox));

  while (gee_iterator_next (iter)) {
    FolksIndividual *individual = gee_iterator_get (iter);

    if (individual == NULL) {
      continue;
    }

    id = folks_individual_get_id (individual);      

    for (l = rows; l; l = l->next) {
      g_object_get (l->data, "id", &row_id, NULL);

      if (!g_strcmp0 (id, row_id ) && l->data != NULL) {
        gtk_widget_destroy (GTK_WIDGET(l->data));
      }
    } 
  
    g_free (row_id);
    g_list_free (l);
    g_clear_object (&individual);
  } 

  g_list_free (rows);
  g_clear_object (&iter);

  iter = gee_iterable_iterator (GEE_ITERABLE (added));

  while (gee_iterator_next (iter)) {
    FolksIndividual *individual = gee_iterator_get (iter);

    if (individual == NULL) {
      continue;
    }

    chatty_folks_individual_add_contact_rows (individual);
  
    g_clear_object (&individual);
  }

  gtk_list_box_invalidate_sort (chatty_folks->listbox);

  g_clear_object (&iter);
  g_object_unref (added);
  g_object_unref (removed);
}


static void
cb_pixbuf_from_stream_ready (GObject      *source_object,
                             GAsyncResult *result,
                             gpointer      avatar_data)
{
  GdkPixbuf   *pixbuf;
  AvatarData  *data = avatar_data;
  PurpleBuddy *buddy;

  g_autoptr(GError) error = NULL;

  pixbuf = gdk_pixbuf_new_from_stream_finish (result, &error);

  if (error != NULL) {
    g_debug ("Could not get pixbuf from stream: %s", error->message);

    g_slice_free (AvatarData, data);

    return;
  }

  switch (data->mode) {
    case CHATTY_FOLKS_SET_CONTACT_ROW_ICON:

      g_return_if_fail (data->row != NULL);
      g_object_set (data->row,
                    "avatar", chatty_folks_shape_pixbuf (pixbuf),
                    NULL);
      break;

    case CHATTY_FOLKS_SET_PURPLE_BUDDY_ICON:
      gdk_pixbuf_save (pixbuf, "/var/tmp/chatty_tmp.jpg", "jpeg", &error, "quality", "100", NULL);

      buddy = purple_find_buddy (data->purple_account, data->purple_user_name);

      purple_buddy_icons_node_set_custom_icon_from_file ((PurpleBlistNode*)buddy,
                                                         "/var/tmp/chatty_tmp.jpg");

      if (error != NULL) {
        g_debug ("Could not save pixbuf to file: %s", error->message);

        g_slice_free (AvatarData, data);

        return;
      }

      remove ("/var/tmp/chatty_tmp.jpg");

      break;

    default:
      g_debug ("Chatty folks icon mode not set");
  }
  
  g_object_unref (pixbuf);
  g_object_unref (data->stream);

  g_slice_free (AvatarData, data);
}


static void
cb_icon_load_async_ready (GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      avatar_data)
{
  GLoadableIcon *icon = G_LOADABLE_ICON (source_object);
  AvatarData    *data = avatar_data;

  g_autoptr(GError) error = NULL;

  data->stream = g_loadable_icon_load_finish (icon, result, NULL, &error);

  if (error != NULL) {
    g_debug ("Could not load icon: %s", error->message);

    g_slice_free (AvatarData, data);

    return;
  }

  gdk_pixbuf_new_from_stream_at_scale_async (data->stream,
                                             data->size, 
                                             data->size, 
                                             TRUE,
                                             NULL,
                                             cb_pixbuf_from_stream_ready, 
                                             data);
}


static GdkPixbuf *
chatty_folks_shape_pixbuf (GdkPixbuf *pixbuf)
{
  cairo_format_t   format;
  cairo_surface_t *surface;
  cairo_t         *cr;
  GdkPixbuf       *ret;
  int              width, height, size;

  format = CAIRO_FORMAT_ARGB32;

  width = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);

  size = (width >= height) ? width : height;

  surface = cairo_image_surface_create (format, size, size);

  cr = cairo_create (surface);

  cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);

  cairo_arc (cr,
             size / 2,
             size / 2,
             size / 2,
             0,
             2 * M_PI);

  cairo_fill (cr);

  gdk_cairo_set_source_pixbuf (cr, pixbuf, 0, 0);

  cairo_arc (cr,
             size / 2,
             size / 2,
             size / 2,
             0,
             2 * M_PI);

  cairo_clip (cr);
  cairo_paint (cr);

  ret = gdk_pixbuf_get_from_surface (surface,
                                     0,
                                     0,
                                     size,
                                     size);

  cairo_surface_destroy (surface);
  cairo_destroy (cr);

  return ret;
}


static void
chatty_folks_load_avatar (FolksIndividual  *individual,
                          ChattyContactRow *row,
                          PurpleAccount    *account,
                          const char       *user_name,
                          int               mode,
                          int               size)
{
  AvatarData    *data;
  GLoadableIcon *avatar;

  g_return_if_fail (FOLKS_IS_INDIVIDUAL (individual));

  avatar = folks_avatar_details_get_avatar (FOLKS_AVATAR_DETAILS(individual));

  if (avatar == NULL) {
    g_debug ("Could not get folks avatar");
    
    return;
  }

  data = g_slice_new0 (AvatarData);
  data->row = row;
  data->mode = mode;
  data->size = size;
  data->purple_account = account;
  data->purple_user_name = user_name;

  g_loadable_icon_load_async (avatar, size, NULL, cb_icon_load_async_ready, data);
}


static gboolean
chatty_folks_individual_has_phonenumber (FolksIndividual *individual,
                                         const char      *phone_number)
{
  GeeSet            *phone_numbers;
  GeeIterator       *iter;
  FolksPhoneDetails *phone_details;
  gboolean           result = FALSE;

  g_return_val_if_fail (FOLKS_IS_INDIVIDUAL (individual), FALSE);

  phone_details = FOLKS_PHONE_DETAILS (individual);
  phone_numbers = folks_phone_details_get_phone_numbers (phone_details);
  iter = gee_iterable_iterator (GEE_ITERABLE(phone_numbers));

  while (gee_iterator_next (iter)) {
    FolksPhoneFieldDetails *field_details;
    char                   *num_e164_1, *num_e164_2;
    char                   *number;

    field_details = gee_iterator_get (iter);
    number = folks_phone_field_details_get_normalised (field_details);

    num_e164_1 = chatty_utils_format_phonenumber (phone_number);
    num_e164_2 = chatty_utils_format_phonenumber (number);

    if (!g_strcmp0 (num_e164_1, num_e164_2)) {
      result = TRUE;
    }

    g_free (number);
    g_free (num_e164_1);
    g_free (num_e164_2);
  }

  return result;
}


/**
 * chatty_folks_has_individual_with_phonenumber:
 * 
 * @number: a const char with the phone number
 * 
 * Lookup a folks individual by phone number
 *
 * Returns: the id of the individual or NULL
 * 
 */
const char *
chatty_folks_has_individual_with_phonenumber (const char *number) 
{ 
  FolksIndividual *individual;
  GeeMapIterator  *iter;

  chatty_folks_data_t *chatty_folks = chatty_get_folks_data ();

  if (chatty_folks->individuals == NULL) {
    return NULL;
  }

  iter = gee_map_map_iterator (chatty_folks->individuals);

  while (gee_map_iterator_next (iter)) {
    individual = gee_map_iterator_get_value (iter);

    if (chatty_folks_individual_has_phonenumber (individual, number)) {
      return gee_map_iterator_get_key (iter);
    }
  }

  return NULL;
}


/**
 * chatty_folks_has_individual_with_name:
 * 
 * @name: a const char with the name of an individual
 * 
 * Check if an individual with a given name is available
 *
 * Returns: the id of the individual or NULL
 */
const char *
chatty_folks_has_individual_with_name (const char *name) 
{ 
  FolksIndividual *individual;
  GeeMapIterator  *iter;
  const char      *folks_name;

  chatty_folks_data_t *chatty_folks = chatty_get_folks_data ();

  if (chatty_folks->individuals == NULL) {
    return NULL;
  }

	iter = gee_map_map_iterator (chatty_folks->individuals);

  while (gee_map_iterator_next (iter)) {
    individual = gee_map_iterator_get_value (iter);

    folks_name = folks_individual_get_display_name (individual);

    if (!g_strcmp0 (folks_name, name)) {
      return gee_map_iterator_get_key (iter);
    }
  }

  return NULL;
}


/**
 * chatty_folks_get_individual_name_by_id:
 * 
 * @id: a const char with the ID of an individual
 * 
 * Get the name of an individual by its ID
 *
 * Returns: the name of the individual or NULL
 */
const char *
chatty_folks_get_individual_name_by_id (const char *id) 
{ 
  FolksIndividual *individual;

  chatty_folks_data_t *chatty_folks = chatty_get_folks_data ();

  individual = FOLKS_INDIVIDUAL(gee_map_get (chatty_folks->individuals, id));

  if (individual != NULL) {
    return folks_individual_get_display_name (individual);
  }

  return NULL;
}


/**
 * chatty_folks_set_purple_buddy_avatar:
 * 
 * @folks_id:  a const char with the ID of an individual
 * @account:   a purple account
 * @user_name: a purple user name
 * 
 * Set a buddy icon from folks avatar data
 *
 */
void
chatty_folks_set_purple_buddy_avatar (const char    *folks_id,
                                      PurpleAccount *account,
                                      const char    *user_name)
{ 
  FolksIndividual *individual;

  chatty_folks_data_t *chatty_folks = chatty_get_folks_data ();

  individual = FOLKS_INDIVIDUAL(gee_map_get (chatty_folks->individuals, folks_id));

  if (individual != NULL) {
    chatty_folks_load_avatar (individual, 
                              NULL, 
                              account, 
                              user_name, 
                              CHATTY_FOLKS_SET_PURPLE_BUDDY_ICON, 
                              48);
  }
}


static const gchar *
chatty_folks_get_phone_type (FolksPhoneFieldDetails *details)
{
  GeeCollection *types;
  GeeIterator   *iter;
  const gchar   *val;

  types = folks_abstract_field_details_get_parameter_values (
    FOLKS_ABSTRACT_FIELD_DETAILS (details), "type");

  if (types == NULL) {
    return NULL;
  }

  iter = gee_iterable_iterator (GEE_ITERABLE (types));

  while (gee_iterator_next (iter)) {
    gchar *type = gee_iterator_get (iter);

    if (!g_strcmp0 (type, "cell")) {
      val = _("Mobile");
    } else if (!g_strcmp0  (type, "work")) {
      val = _("Work");
    } else if (!g_strcmp0  (type, "home")) {
      val = _("Home");
    } else {
      val = NULL;
    }

    g_free (type);
  }

  g_object_unref (iter);

  return val;
}


/**
 * chatty_folks_individual_add_contact_rows:
 * 
 * @individual: a FolksIndividual
 * 
 * Creates a ChattyContactRow with the name 
 * and phone number of an folks individual 
 * and adds it to the list that has been 
 * passed to #chatty_folks_init.
 * The EDS contacts will be available only
 * in the contacts list, without adding them
 * to blist.xml
 * 
 * Called from: #chatty_folks_populate_contact_list
 *
 */
static void
chatty_folks_individual_add_contact_rows (FolksIndividual *individual)
{
  ChattyContactRow  *row;
  GeeSet            *phone_numbers;
  GeeIterator       *iter;
  FolksPhoneDetails *phone_details;
  const char        *folks_id;
  const char        *name;

  chatty_folks_data_t *chatty_folks = chatty_get_folks_data ();

  g_return_if_fail (FOLKS_IS_INDIVIDUAL (individual));

  phone_details = FOLKS_PHONE_DETAILS (individual);
  phone_numbers = folks_phone_details_get_phone_numbers (phone_details);
  iter = gee_iterable_iterator (GEE_ITERABLE(phone_numbers));

  folks_id = folks_individual_get_id (individual);
  name = folks_individual_get_display_name (individual);

  while (gee_iterator_next (iter)) {
    FolksPhoneFieldDetails *field_details;
    g_autofree char        *number;
    char                   *type_number;

    field_details = gee_iterator_get (iter);
    number = folks_phone_field_details_get_normalised (field_details);

    type_number = g_strconcat (chatty_folks_get_phone_type (field_details),
                               ": ",
                               number, 
                               NULL);

    row = CHATTY_CONTACT_ROW (chatty_contact_row_new (NULL,
                                                      NULL,
                                                      name,
                                                      type_number,
                                                      NULL,
                                                      NULL,
                                                      folks_id,
                                                      number));

    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);

    gtk_container_add (GTK_CONTAINER (chatty_folks->listbox), GTK_WIDGET (row));

    gtk_widget_show (GTK_WIDGET (row));

    chatty_folks_load_avatar (individual, 
                              row, 
                              NULL,
                              NULL,
                              CHATTY_FOLKS_SET_CONTACT_ROW_ICON, 
                              36);

    g_object_unref (field_details);
  }

  g_object_unref (iter);
}


/**
 * chatty_folks_init:
 * 
 * @listbox: a GtkListBox contacts will be added to
 * 
 * Prepare the folks aggregator
 */
void
chatty_folks_init (GtkListBox *listbox)
{
  chatty_folks_data_t *chatty_folks = chatty_get_folks_data ();

  chatty_folks->listbox = listbox;

  chatty_folks->aggregator = folks_individual_aggregator_dup ();

  g_signal_connect_object (G_OBJECT(chatty_folks->aggregator),
                           "notify::is-quiescent",
                           G_CALLBACK(cb_aggregator_notify),
                           NULL,
                           0);

  g_signal_connect_object (G_OBJECT(chatty_folks->aggregator),
                           "individuals-changed-detailed",
                           G_CALLBACK (cb_aggregator_individuals_changed), 
                           NULL, 
                           0);

  folks_individual_aggregator_prepare (chatty_folks->aggregator,  
                                       (GAsyncReadyCallback)cb_aggregator_prepare_finish,
                                       NULL);
}


void
chatty_folks_close (void)
{
  chatty_folks_data_t *chatty_folks = chatty_get_folks_data ();

  g_clear_object (&chatty_folks->aggregator);
}