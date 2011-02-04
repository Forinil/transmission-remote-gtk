/*
 * transmission-remote-gtk - A GTK RPC client to Transmission
 * Copyright (C) 2011  Alan Fitton

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <gdk/gdkkeysyms.h>
#include <curl/curl.h>
#include <libnotify/notify.h>

#include "dispatch.h"
#include "trg-client.h"
#include "http.h"
#include "json.h"
#include "util.h"
#include "requests.h"
#include "session-get.h"

#include "trg-main-window.h"
#include "trg-about-window.h"
#include "trg-tree-view.h"
#include "trg-preferences.h"
#include "trg-torrent-model.h"
#include "trg-torrent-tree-view.h"
#include "trg-peers-model.h"
#include "trg-peers-tree-view.h"
#include "trg-files-tree-view.h"
#include "trg-files-model.h"
#include "trg-trackers-tree-view.h"
#include "trg-trackers-model.h"
#include "trg-state-selector.h"
#include "trg-torrent-props-dialog.h"
#include "trg-torrent-add-url-dialog.h"
#include "trg-toolbar.h"
#include "trg-menu-bar.h"
#include "trg-status-bar.h"
#include "trg-remote-prefs-dialog.h"
#include "trg-preferences-dialog.h"

/* Events */

static void on_torrent_get(JsonObject * response, int status,
			   gpointer data);

static void on_torrent_get_first(JsonObject * response, int status,
				 gpointer data);

static void on_torrent_get_multipurpose(JsonObject * response,
					gboolean first, int status,
					gpointer data);

static void trg_main_window_conn_changed(TrgMainWindow * win,
					 gboolean connected);
static void
trg_main_window_update_notebook_displays(TrgMainWindow * win,
					 JsonObject * t,
					 GtkTreeIter * iter,
					 gboolean first);

static gboolean torrent_selection_changed(GtkWidget * w, gpointer data);

static gboolean trg_update_torrents_timerfunc(gpointer data);

static void destroy_window();

static void on_torrent_completed(TrgTorrentModel * model,
				 GtkTreeIter * iter, gpointer * data);

static void on_session_get(JsonObject * response, int status,
			   gpointer data);

static void entry_filter_changed_cb(GtkWidget * w, gpointer data);

/* Actions */

static void connect_cb(GtkWidget * w, gpointer data);
static void disconnect_cb(GtkWidget * w, gpointer data);
static void add_cb(GtkWidget * w, gpointer data);
static void resume_cb(GtkWidget * w, gpointer data);
static void pause_cb(GtkWidget * w, gpointer data);
static void verify_cb(GtkWidget * w, gpointer data);
static void remove_cb(GtkWidget * w, gpointer data);
static void delete_cb(GtkWidget * w, gpointer data);
static void open_about_cb(GtkWidget * w, GtkWindow * parent);

/* Utility */

static
void trg_main_window_torrent_scrub(TrgMainWindow * win);
static const gchar *make_error_message(JsonObject * response, int status);
static GtkWidget *my_scrolledwin_new(GtkWidget * child);
static gboolean
trg_dialog_error_handler(TrgMainWindow * win, JsonObject * response,
			 int status);
static void response_unref(JsonObject * response);

G_DEFINE_TYPE(TrgMainWindow, trg_main_window, GTK_TYPE_WINDOW)
#define TRG_MAIN_WINDOW_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRG_TYPE_MAIN_WINDOW, TrgMainWindowPrivate))
typedef struct _TrgMainWindowPrivate TrgMainWindowPrivate;

struct _TrgMainWindowPrivate {
    trg_client *client;
    TrgToolbar *toolBar;
    TrgMenuBar *menuBar;

    TrgStatusBar *statusBar;
    GtkStatusIcon *statusIcon;
    TrgStateSelector *stateSelector;
    TrgGeneralPanel *genDetails;
    GtkWidget *notebook;

    TrgTorrentModel *torrentModel;
    TrgTorrentTreeView *torrentTreeView;
    GtkTreeModel *filteredTorrentModel;
    GtkTreeModel *sortedTorrentModel;
    gint selectedTorrentId;

    TrgTrackersModel *trackersModel;
    TrgTrackersTreeView *trackersTreeView;

    TrgFilesModel *filesModel;
    TrgFilesTreeView *filesTreeView;

    TrgPeersModel *peersModel;
    TrgPeersTreeView *peersTreeView;

    GtkWidget *hpaned, *vpaned;
    GtkWidget *filterEntry, *filterEntryClearButton;
};

enum {
    PROP_0,
    PROP_CLIENT
};

static void trg_main_window_init(TrgMainWindow * self G_GNUC_UNUSED)
{
}

static gboolean update_selected_torrent_notebook(TrgMainWindow * win,
						 gboolean first)
{
    TrgMainWindowPrivate *priv;
    GtkTreeIter iter;
    gint newFirstSelected;
    JsonObject *json = NULL;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(win);
    newFirstSelected =
	get_first_selected(priv->torrentTreeView, &iter, &json);

    if (priv->selectedTorrentId >= 0
	&& (priv->selectedTorrentId != newFirstSelected
	    || newFirstSelected < 0)) {
	trg_main_window_torrent_scrub(win);
    }

    if ((priv->selectedTorrentId = newFirstSelected) >= 0) {
	trg_main_window_update_notebook_displays(win, json, &iter, first);
	return TRUE;
    }

    return FALSE;
}

static void response_unref(JsonObject * response)
{
    if (response != NULL)
	json_object_unref(response);
}

static void on_torrent_completed(TrgTorrentModel * model,
				 GtkTreeIter * iter, gpointer * data)
{
    TrgMainWindowPrivate *priv;
    gchar *name;
    NotifyNotification *notify;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    if (!gtk_status_icon_is_embedded(priv->statusIcon))
	return;

    gtk_tree_model_get(GTK_TREE_MODEL(model), iter,
		       TORRENT_COLUMN_NAME, &name, -1);

    notify =
	notify_notification_new(name,
				"This torrent has finished downloading",
				GTK_STOCK_APPLY
/* the fourth argument was removed in libnotify 0.7.0 */
#if !defined(NOTIFY_VERSION_MINOR) || (NOTIFY_VERSION_MAJOR == 0 && NOTIFY_VERSION_MINOR < 7)
                                               , NULL
#endif
                                                     );

#if !defined(NOTIFY_VERSION_MINOR) || (NOTIFY_VERSION_MAJOR == 0 && NOTIFY_VERSION_MINOR < 7)
                notify_notification_attach_to_status_icon(notify, priv->statusIcon);
#endif



    notify_notification_set_urgency(notify, NOTIFY_URGENCY_LOW);
    notify_notification_set_timeout(notify, 8000);

    g_free(name);

    notify_notification_show(notify, NULL);
}

static gboolean delete_event(GtkWidget * w,
                             GdkEvent * event G_GNUC_UNUSED,
			     gpointer * data G_GNUC_UNUSED)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(w);
    int width, height;
    gtk_window_get_size(GTK_WINDOW(w), &width, &height);
    gconf_client_set_int(priv->client->gconf, TRG_GCONF_KEY_WINDOW_HEIGHT,
			 height, NULL);
    gconf_client_set_int(priv->client->gconf, TRG_GCONF_KEY_WINDOW_WIDTH,
			 width, NULL);

    return FALSE;
}

static void
destroy_window(GtkWidget * w G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
    gtk_main_quit();
}

static const gchar *make_error_message(JsonObject * response, int status)
{
    if (status == FAIL_JSON_DECODE) {
	return g_strdup("JSON decoding error.");
    } else if (status == FAIL_RESPONSE_UNSUCCESSFUL) {
	const gchar *resultStr =
	    json_object_get_string_member(response, "result");
	if (resultStr == NULL)
	    return g_strdup("Server responded, but with no result.");
	else
	    return g_strdup(resultStr);
    } else if (status <= -100) {
	return g_strdup_printf("Request failed with HTTP code %d",
			       -(status + 100));
    } else {
	return g_strdup(curl_easy_strerror(status));
    }
}

static void open_props_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);
    TrgTorrentPropsDialog *dialog =
	trg_torrent_props_dialog_new(GTK_WINDOW(data),
				     priv->torrentTreeView,
				     priv->client);

    gtk_widget_show_all(GTK_WIDGET(dialog));
}

/* Use synchronous dispatch() in our dedicated thread function.
 * This means torrents are added in sequence, instead of dispatch_async()
 * working concurrently for each upload.
 */

static gpointer add_files_threadfunc(gpointer data)
{
    JsonObject *response;
    JsonNode *request;
    struct add_torrent_threadfunc_args *args;
    gint status;
    GSList *li;

    args = (struct add_torrent_threadfunc_args *) data;

    for (li = args->list; li != NULL; li = g_slist_next(li)) {
	request = torrent_add((gchar *) li->data, FALSE);
	g_free(li->data);
	response = dispatch(args->client, request, &status);
	on_generic_interactive_action(response, status, args->cb_data);
    }

    g_slist_free(args->list);
    g_free(args);

    return NULL;
}

static void add_url_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindow *win = TRG_MAIN_WINDOW(data);
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    TrgTorrentAddUrlDialog *dlg =
	trg_torrent_add_url_dialog_new(win, priv->client);
    gtk_widget_show_all(GTK_WIDGET(dlg));
}

static void add_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv;
    GtkWidget *dialog;
    GtkFileFilter *filter;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    dialog = gtk_file_chooser_dialog_new("Open File",
					 GTK_WINDOW(data),
					 GTK_FILE_CHOOSER_ACTION_OPEN,
					 GTK_STOCK_CANCEL,
					 GTK_RESPONSE_CANCEL,
					 GTK_STOCK_OPEN,
					 GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

    filter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(filter, "*.torrent");
    gtk_file_filter_set_name(filter, "BitTorrent Metadata");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
	GThread *thread;
	GError *error = NULL;
	struct add_torrent_threadfunc_args *args;

	args = g_new(struct add_torrent_threadfunc_args, 1);
	args->list =
	    gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
	args->cb_data = data;
	args->client = priv->client;

	thread =
	    g_thread_create(add_files_threadfunc, args, FALSE, &error);
	if (error != NULL) {
	    g_printf("thread creation error: %s\n", error->message);
	    g_error_free(error);
	    g_free(args);
	}
    }

    gtk_widget_destroy(dialog);
}

static void pause_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    dispatch_async(priv->client,
		   torrent_pause(build_json_id_array
				 (priv->torrentTreeView)),
		   on_generic_interactive_action, data);
}

gboolean trg_add_from_filename(TrgMainWindow * win, gchar * fileName)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(win);

    if (g_file_test(fileName, G_FILE_TEST_EXISTS) == TRUE) {
	JsonNode *torrentAddReq = torrent_add(fileName, FALSE);
	dispatch_async(priv->client, torrentAddReq,
		       on_generic_interactive_action, win);
	return TRUE;
    }

    return FALSE;
}

static void resume_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    dispatch_async(priv->client,
		   torrent_start(build_json_id_array
				 (priv->torrentTreeView)),
		   on_generic_interactive_action, data);
}

static void disconnect_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    trg_main_window_conn_changed(TRG_MAIN_WINDOW(data), FALSE);
    trg_status_bar_push_connection_msg(priv->statusBar, "Disconnected.");
}

static void connect_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    if (trg_client_populate_with_settings
	(priv->client, priv->client->gconf) == FALSE) {
	GtkWidget *dialog;

	trg_status_bar_push_connection_msg(priv->statusBar,
					   "Unable to get gconf settings.");

	dialog =
	    gtk_message_dialog_new(GTK_WINDOW(data),
				   GTK_DIALOG_DESTROY_WITH_PARENT,
				   GTK_MESSAGE_ERROR,
				   GTK_BUTTONS_OK,
				   "Unable to retrieve connection settings from GConf. Schema not installed?");
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	return;
    }

    trg_status_bar_push_connection_msg(priv->statusBar, "Connecting...");
    dispatch_async(priv->client, session_get(), on_session_get, data);
}

static void open_local_prefs_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    GtkWidget *dlg = trg_preferences_dialog_get_instance(GTK_WINDOW(data),
							 priv->client->
							 gconf);
    gtk_widget_show_all(dlg);
}

static void open_remote_prefs_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    TrgRemotePrefsDialog *dlg =
	trg_remote_prefs_dialog_get_instance(TRG_MAIN_WINDOW(data),
					     priv->client);

    gtk_widget_show_all(GTK_WIDGET(dlg));
}

static TrgToolbar *trg_main_window_toolbar_new(TrgMainWindow * win)
{
    GObject *b_connect, *b_disconnect, *b_add, *b_resume, *b_pause;
    GObject *b_remove, *b_delete, *b_props, *b_local_prefs,
	*b_remote_prefs;

    TrgToolbar *toolBar = trg_toolbar_new();
    g_object_get(toolBar,
		 "connect-button", &b_connect,
		 "disconnect-button", &b_disconnect,
		 "add-button", &b_add,
		 "resume-button", &b_resume,
		 "pause-button", &b_pause,
		 "delete-button", &b_delete,
		 "remove-button", &b_remove,
		 "props-button", &b_props,
		 "remote-prefs-button", &b_remote_prefs,
		 "local-prefs-button", &b_local_prefs, NULL);

    g_signal_connect(b_connect, "clicked", G_CALLBACK(connect_cb), win);
    g_signal_connect(b_disconnect, "clicked",
		     G_CALLBACK(disconnect_cb), win);
    g_signal_connect(b_add, "clicked", G_CALLBACK(add_cb), win);
    g_signal_connect(b_resume, "clicked", G_CALLBACK(resume_cb), win);
    g_signal_connect(b_pause, "clicked", G_CALLBACK(pause_cb), win);
    g_signal_connect(b_delete, "clicked", G_CALLBACK(delete_cb), win);
    g_signal_connect(b_remove, "clicked", G_CALLBACK(remove_cb), win);
    g_signal_connect(b_props, "clicked", G_CALLBACK(open_props_cb), win);
    g_signal_connect(b_local_prefs, "clicked",
		     G_CALLBACK(open_local_prefs_cb), win);
    g_signal_connect(b_remote_prefs, "clicked",
		     G_CALLBACK(open_remote_prefs_cb), win);

    return toolBar;
}

static void verify_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    g_printf("verify_cb()\n");
    dispatch_async(priv->client,
		   torrent_verify(build_json_id_array
				  (priv->torrentTreeView)),
		   on_generic_interactive_action, data);
}

static gint confirm_action_dialog(GtkWindow * win,
				  GtkTreeSelection * selection,
				  gchar * question_single,
				  gchar * question_multi,
				  gchar * action_stock)
{
    TrgMainWindowPrivate *priv;
    gint selectCount;
    gint response;
    GtkWidget *dialog = NULL;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(win);

    selectCount = gtk_tree_selection_count_selected_rows(selection);

    if (selectCount == 1) {
	GList *list;
	GList *firstNode;
	GtkTreeIter firstIter;
	gchar *name = NULL;

	list = gtk_tree_selection_get_selected_rows(selection, NULL);
	firstNode = g_list_first(list);

	gtk_tree_model_get_iter(GTK_TREE_MODEL
				(priv->sortedTorrentModel),
				&firstIter, firstNode->data);
	gtk_tree_model_get(GTK_TREE_MODEL
			   (priv->sortedTorrentModel),
			   &firstIter, TORRENT_COLUMN_NAME, &name, -1);
	g_list_free(list);

	dialog =
	    gtk_message_dialog_new_with_markup(GTK_WINDOW(win),
					       GTK_DIALOG_DESTROY_WITH_PARENT,
					       GTK_MESSAGE_QUESTION,
					       GTK_BUTTONS_NONE,
					       question_single, name);
	g_free(name);
    } else if (selectCount > 1) {
	dialog =
	    gtk_message_dialog_new_with_markup(GTK_WINDOW(win),
					       GTK_DIALOG_DESTROY_WITH_PARENT,
					       GTK_MESSAGE_QUESTION,
					       GTK_BUTTONS_NONE,
					       question_multi,
					       selectCount);

    } else {
	return 0;
    }

    gtk_dialog_add_buttons(GTK_DIALOG(dialog),
			   GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			   action_stock, GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog),
				    GTK_RESPONSE_CANCEL);
    gtk_dialog_set_alternative_button_order(GTK_DIALOG(dialog),
					    GTK_RESPONSE_ACCEPT,
					    GTK_RESPONSE_CANCEL, -1);

    response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response;
}

static void remove_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv;
    GtkTreeSelection *selection;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    selection =
	gtk_tree_view_get_selection(GTK_TREE_VIEW(priv->torrentTreeView));

    if (confirm_action_dialog
	(GTK_WINDOW(data), selection,
	 "<big><b>Remove torrent \"%s\"?</b></big>",
	 "<big><b>Remove %d torrents?</b></big>",
	 GTK_STOCK_REMOVE) == GTK_RESPONSE_ACCEPT)
	dispatch_async(priv->client,
		       torrent_remove(build_json_id_array
				      (priv->torrentTreeView),
				      FALSE),
		       on_generic_interactive_action, data);
}

static void delete_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindowPrivate *priv;
    GtkTreeSelection *selection;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    selection =
	gtk_tree_view_get_selection(GTK_TREE_VIEW(priv->torrentTreeView));

    if (confirm_action_dialog
	(GTK_WINDOW(data), selection,
	 "<big><b>Remove and delete torrent \"%s\"?</b></big>",
	 "<big><b>Remove and delete %d torrents?</b></big>",
	 GTK_STOCK_DELETE) == GTK_RESPONSE_ACCEPT)
	dispatch_async(priv->client,
		       torrent_remove(build_json_id_array
				      (priv->torrentTreeView),
				      TRUE),
		       on_generic_interactive_action, data);
}

static
GtkWidget *my_scrolledwin_new(GtkWidget * child)
{
    GtkWidget *scrolled_win = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_win),
				   GTK_POLICY_AUTOMATIC,
				   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled_win), child);
    return scrolled_win;
}

static void view_states_toggled_cb(GtkCheckMenuItem * w, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);
    gtk_widget_set_visible(GTK_WIDGET(priv->stateSelector),
			   gtk_check_menu_item_get_active(w));
}

static void view_notebook_toggled_cb(GtkCheckMenuItem * w, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);
    gtk_widget_set_visible(priv->notebook,
			   gtk_check_menu_item_get_active(w));
}

static
GtkWidget *trg_main_window_notebook_new(TrgMainWindow * win)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(win);

    GtkWidget *notebook = gtk_notebook_new();

    gtk_widget_set_size_request(notebook, -1, 200);

    priv->genDetails = trg_general_panel_new(priv->sortedTorrentModel);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
			     GTK_WIDGET(priv->genDetails),
			     gtk_label_new("General"));

    priv->trackersModel = trg_trackers_model_new();
    priv->trackersTreeView =
	trg_trackers_tree_view_new(priv->trackersModel);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
			     my_scrolledwin_new(GTK_WIDGET
						(priv->trackersTreeView)),
			     gtk_label_new("Trackers"));

    priv->filesModel = trg_files_model_new();
    priv->filesTreeView =
	trg_files_tree_view_new(priv->filesModel, win, priv->client);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
			     my_scrolledwin_new(GTK_WIDGET
						(priv->filesTreeView)),
			     gtk_label_new("Files"));

    priv->peersModel = trg_peers_model_new();
    priv->peersTreeView = trg_peers_tree_view_new(priv->peersModel);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
			     my_scrolledwin_new(GTK_WIDGET
						(priv->peersTreeView)),
			     gtk_label_new("Peers"));

    return notebook;
}

void on_session_set(JsonObject * response, int status, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    gdk_threads_enter();
    trg_dialog_error_handler(TRG_MAIN_WINDOW(data), response, status);
    gdk_threads_leave();

    response_unref(response);

    if (status == CURLE_OK || status == FAIL_RESPONSE_UNSUCCESSFUL)
	dispatch_async(priv->client, session_get(), on_session_get, data);
}

static void on_session_get(JsonObject * response, int status,
			   gpointer data)
{
    TrgMainWindow *win = TRG_MAIN_WINDOW(data);
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);
    trg_client *client = priv->client;
    JsonObject *newSession;

    gdk_threads_enter();

    if (trg_dialog_error_handler(win, response, status) == TRUE) {
	response_unref(response);
	gdk_threads_leave();
	return;
    }

    newSession = get_arguments(response);

    if (client->session != NULL) {
	json_object_unref(client->session);
    } else {
	trg_status_bar_connect(priv->statusBar, newSession);
	trg_main_window_conn_changed(win, TRUE);

	dispatch_async(client, torrent_get(), on_torrent_get_first, data);
    }

    client->session = newSession;

    gdk_threads_leave();
    json_object_ref(newSession);
    response_unref(response);
}

static void
on_torrent_get_first(JsonObject * response, int status, gpointer data)
{
    on_torrent_get_multipurpose(response, TRUE, status, data);
}

static void on_torrent_get(JsonObject * response, int status,
			   gpointer data)
{
    on_torrent_get_multipurpose(response, FALSE, status, data);
}

static gboolean trg_update_torrents_timerfunc(gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    if (priv->client->session != NULL)
	dispatch_async(priv->client, torrent_get(), on_torrent_get, data);

    return FALSE;
}

static void
trg_main_window_update_notebook_displays(TrgMainWindow * win,
					 JsonObject * t,
					 GtkTreeIter * iter,
					 gboolean first)
{
    TrgMainWindowPrivate *priv;
    trg_client *client;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(win);
    client = priv->client;

    trg_general_panel_update(priv->genDetails, t, iter);

    if (first == TRUE)
	trg_trackers_model_update(priv->trackersModel, t);

    trg_files_model_update(priv->filesModel, client->updateSerial,
			   t, first);
    trg_peers_model_update(priv->peersModel, client->updateSerial,
			   t, first);
}

static void
open_about_cb(GtkWidget * w G_GNUC_UNUSED, GtkWindow * parent)
{
    GtkWidget *aboutDialog = trg_about_window_new(parent);

    gtk_dialog_run(GTK_DIALOG(aboutDialog));
    gtk_widget_destroy(aboutDialog);
}

static gboolean
trg_torrent_tree_view_visible_func(GtkTreeModel * model,
				   GtkTreeIter * iter, gpointer data)
{
    guint flags;
    gchar *name = NULL;
    gboolean visible = TRUE;

    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);

    guint32 criteria = trg_state_selector_get_flag(priv->stateSelector);

    gtk_tree_model_get(model, iter,
		       TORRENT_COLUMN_FLAGS, &flags,
		       TORRENT_COLUMN_NAME, &name, -1);

    if (criteria != 0 && !(flags & criteria)) {
	visible = FALSE;
    } else if (name != NULL) {
	const gchar *filterText =
	    gtk_entry_get_text(GTK_ENTRY(priv->filterEntry));
	if (strlen(filterText) > 0 && strstr(name, filterText) == NULL)
	    visible = FALSE;
    }

    g_free(name);

    return visible;
}

static
TrgTorrentTreeView *trg_main_window_torrent_tree_view_new(
    TrgMainWindow * win,
    GtkTreeModel * model,
    TrgStateSelector * selector G_GNUC_UNUSED)
{
    TrgTorrentTreeView *torrentTreeView = trg_torrent_tree_view_new(model);

    GtkTreeSelection *selection =
	gtk_tree_view_get_selection(GTK_TREE_VIEW(torrentTreeView));

    g_signal_connect(G_OBJECT(selection), "changed",
		     G_CALLBACK(torrent_selection_changed), win);

    return torrentTreeView;
}

static gboolean
trg_dialog_error_handler(TrgMainWindow * win, JsonObject * response,
			 int status)
{
    TrgMainWindowPrivate *priv;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(win);

    if (status != CURLE_OK) {
	GtkWidget *dialog;
	const gchar *msg;

	msg = make_error_message(response, status);
	trg_status_bar_push_connection_msg(priv->statusBar, msg);
	dialog = gtk_message_dialog_new(GTK_WINDOW(win),
					GTK_DIALOG_MODAL,
					GTK_MESSAGE_ERROR,
					GTK_BUTTONS_OK, msg);
	gtk_window_set_title(GTK_WINDOW(dialog), "Error");
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	g_free((gpointer) msg);
	return TRUE;
    } else {
	return FALSE;
    }
}

static gboolean
torrent_selection_changed(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    TrgMainWindow *win;
    TrgMainWindowPrivate *priv;
    gboolean isSelected;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);
    win = TRG_MAIN_WINDOW(data);

    isSelected = update_selected_torrent_notebook(win, TRUE);

    trg_toolbar_torrent_actions_sensitive(priv->toolBar, isSelected);
    trg_menu_bar_torrent_actions_sensitive(priv->menuBar, isSelected);

    return TRUE;
}

void
on_generic_interactive_action(JsonObject * response, int status,
			      gpointer data)
{
    gdk_threads_enter();
    trg_dialog_error_handler(TRG_MAIN_WINDOW(data), response, status);
    gdk_threads_leave();

    response_unref(response);
}

static
void trg_main_window_torrent_scrub(TrgMainWindow * win)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(win);

    gtk_list_store_clear(GTK_LIST_STORE(priv->filesModel));
    gtk_list_store_clear(GTK_LIST_STORE(priv->trackersModel));
    gtk_list_store_clear(GTK_LIST_STORE(priv->peersModel));
    trg_general_panel_clear(priv->genDetails);
}

static void
on_torrent_get_multipurpose(JsonObject * response, gboolean first,
			    int status, gpointer data)
{
    TrgTorrentModelClassUpdateStats stats;
    TrgMainWindowPrivate *priv;
    trg_client *client;

    priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);
    client = priv->client;

    g_mutex_lock(client->updateMutex);
    gdk_threads_enter();

    /* Disconnected between request and response callback */
    if (client->session == NULL) {
	gdk_threads_leave();
	g_mutex_unlock(client->updateMutex);
	response_unref(response);
	return;
    }

    if (status != CURLE_OK) {
	client->failCount++;
	if (client->failCount >= 3) {
	    trg_main_window_conn_changed(TRG_MAIN_WINDOW(data), FALSE);
	    trg_dialog_error_handler(TRG_MAIN_WINDOW(data),
				     response, status);
	} else {
	    const gchar *msg;
	    gchar *statusBarMsg;

	    msg = make_error_message(response, status);
	    statusBarMsg =
		g_strdup_printf("Request %d/%d failed: %s",
				client->failCount, 3, msg);
	    trg_status_bar_push_connection_msg(priv->statusBar,
					       statusBarMsg);
	    g_free((gpointer) msg);
	    g_free(statusBarMsg);
	    g_timeout_add_seconds(3, trg_update_torrents_timerfunc, data);
	}
	gdk_threads_leave();
	g_mutex_unlock(client->updateMutex);
	response_unref(response);
	return;
    }

    client->failCount = 0;
    stats.downRateTotal = 0;
    stats.upRateTotal = 0;
    stats.seeding = 0;
    stats.down = 0;
    stats.paused = 0;

    trg_torrent_model_update(priv->torrentModel, priv->client,
			     response, &stats, first);

    trg_status_bar_update(priv->statusBar, &stats);

    update_selected_torrent_notebook(TRG_MAIN_WINDOW(data), first);

    g_timeout_add_seconds(3, trg_update_torrents_timerfunc, data);

    client->updateSerial++;

    gdk_threads_leave();
    g_mutex_unlock(client->updateMutex);
    response_unref(response);
}

static void entry_filter_changed_cb(GtkWidget * w, gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);
    gboolean clearSensitive = gtk_entry_get_text_length(GTK_ENTRY(w)) > 0;

    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER
				   (priv->filteredTorrentModel));

#if GTK_CHECK_VERSION( 2,16,0 )
    g_object_set(priv->filterEntryClearButton,
		 "secondary-icon-sensitive", clearSensitive, NULL);
#else
    gtk_widget_set_sensitive(priv->filterEntryClearButton, clearSensitive);
#endif
}

static void
torrent_state_selection_changed(TrgStateSelector * selector G_GNUC_UNUSED,
                                guint flag G_GNUC_UNUSED,
                                gpointer data)
{
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(data));
}

static
void trg_main_window_conn_changed(TrgMainWindow * win, gboolean connected)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(win);
    trg_client *tc = priv->client;

    trg_toolbar_connected_change(priv->toolBar, connected);
    trg_menu_bar_connected_change(priv->menuBar, connected);

    gtk_widget_set_sensitive(GTK_WIDGET(priv->torrentTreeView), connected);
    gtk_widget_set_sensitive(GTK_WIDGET(priv->peersTreeView), connected);
    gtk_widget_set_sensitive(GTK_WIDGET(priv->filesTreeView), connected);
    gtk_widget_set_sensitive(GTK_WIDGET(priv->trackersTreeView),
			     connected);
    gtk_widget_set_sensitive(GTK_WIDGET(priv->genDetails), connected);

    if (connected == FALSE) {
	json_object_unref(tc->session);
	tc->session = NULL;

	gtk_list_store_clear(GTK_LIST_STORE(priv->torrentModel));
	trg_main_window_torrent_scrub(win);
    }
}

static void
trg_main_window_get_property(GObject * object, guint property_id,
			     GValue * value, GParamSpec * pspec)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(object);
    switch (property_id) {
    case PROP_CLIENT:
	g_value_set_pointer(value, priv->client);
	break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
trg_main_window_set_property(GObject * object, guint property_id,
			     const GValue * value, GParamSpec * pspec)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(object);
    switch (property_id) {
    case PROP_CLIENT:
	priv->client = g_value_get_pointer(value);
	break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void quit_cb(GtkWidget * w G_GNUC_UNUSED, gpointer data)
{
    gtk_widget_destroy(GTK_WIDGET(data));
}

static TrgMenuBar *trg_main_window_menu_bar_new(TrgMainWindow * win)
{
    GObject *b_connect, *b_disconnect, *b_add, *b_resume, *b_pause,
	*b_verify, *b_remove, *b_delete, *b_props, *b_local_prefs,
	*b_remote_prefs, *b_about, *b_view_states, *b_view_notebook,
	*b_add_url, *b_quit;
    TrgMenuBar *menuBar;

    menuBar = trg_menu_bar_new(win);
    g_object_get(menuBar,
		 "connect-button", &b_connect,
		 "disconnect-button", &b_disconnect,
		 "add-button", &b_add,
		 "add-url-button", &b_add_url,
		 "resume-button", &b_resume,
		 "pause-button", &b_pause,
		 "delete-button", &b_delete,
		 "remove-button", &b_remove,
		 "verify-button", &b_verify,
		 "props-button", &b_props,
		 "remote-prefs-button", &b_remote_prefs,
		 "local-prefs-button", &b_local_prefs,
		 "view-notebook-button", &b_view_notebook,
		 "view-states-button", &b_view_states,
		 "about-button", &b_about, "quit-button", &b_quit, NULL);

    g_signal_connect(b_connect, "activate", G_CALLBACK(connect_cb), win);
    g_signal_connect(b_disconnect, "activate",
		     G_CALLBACK(disconnect_cb), win);
    g_signal_connect(b_add, "activate", G_CALLBACK(add_cb), win);
    g_signal_connect(b_add_url, "activate", G_CALLBACK(add_url_cb), win);
    g_signal_connect(b_resume, "activate", G_CALLBACK(resume_cb), win);
    g_signal_connect(b_pause, "activate", G_CALLBACK(pause_cb), win);
    g_signal_connect(b_verify, "activate", G_CALLBACK(verify_cb), win);
    g_signal_connect(b_delete, "activate", G_CALLBACK(delete_cb), win);
    g_signal_connect(b_remove, "activate", G_CALLBACK(remove_cb), win);
    g_signal_connect(b_about, "activate", G_CALLBACK(open_about_cb), win);
    g_signal_connect(b_local_prefs, "activate",
		     G_CALLBACK(open_local_prefs_cb), win);
    g_signal_connect(b_remote_prefs, "activate",
		     G_CALLBACK(open_remote_prefs_cb), win);
    g_signal_connect(b_view_notebook, "toggled",
		     G_CALLBACK(view_notebook_toggled_cb), win);
    g_signal_connect(b_view_states, "toggled",
		     G_CALLBACK(view_states_toggled_cb), win);
    g_signal_connect(b_props, "activate", G_CALLBACK(open_props_cb), win);
    g_signal_connect(b_quit, "activate", G_CALLBACK(quit_cb), win);

    return menuBar;
}

static void status_icon_activated(GtkStatusIcon * icon G_GNUC_UNUSED, gpointer data)
{
    gtk_window_present(GTK_WINDOW(data));
}

static void clear_filter_entry_cb(GtkWidget * w, gpointer data G_GNUC_UNUSED)
{
    gtk_entry_set_text(GTK_ENTRY(w), "");
}

static gboolean torrent_tv_key_press_event(GtkWidget * w,
					   GdkEventKey * key,
					   gpointer data)
{
    if (key->keyval == GDK_Delete) {
	if (key->state & GDK_SHIFT_MASK)
	    delete_cb(w, data);
	else
	    remove_cb(w, data);
    }
    return FALSE;
}

static
GtkWidget *trg_imagemenuitem_new(GtkMenuShell * shell, char *text,
				 char *stock_id, gboolean sensitive,
				 GCallback cb, gpointer cbdata)
{
    GtkWidget *item = gtk_image_menu_item_new_with_label(stock_id);

    gtk_image_menu_item_set_use_stock(GTK_IMAGE_MENU_ITEM(item), TRUE);
    gtk_image_menu_item_set_always_show_image(GTK_IMAGE_MENU_ITEM
					      (item), TRUE);
    gtk_menu_item_set_label(GTK_MENU_ITEM(item), text);
    g_signal_connect(item, "activate", cb, cbdata);
    gtk_widget_set_sensitive(item, sensitive);
    gtk_menu_shell_append(shell, item);

    return item;
}

static void
trg_torrent_tv_view_menu(GtkWidget * treeview G_GNUC_UNUSED,
                         GdkEventButton * event,
			 gpointer data)
{
    GtkWidget *menu;

    menu = gtk_menu_new();

    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Properties",
			  GTK_STOCK_PROPERTIES, TRUE,
			  G_CALLBACK(open_props_cb), data);
    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Resume",
			  GTK_STOCK_MEDIA_PLAY, TRUE,
			  G_CALLBACK(resume_cb), data);
    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Pause",
			  GTK_STOCK_MEDIA_PAUSE, TRUE,
			  G_CALLBACK(pause_cb), data);
    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Verify",
			  GTK_STOCK_REFRESH, TRUE, G_CALLBACK(verify_cb),
			  data);
    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Remove", GTK_STOCK_REMOVE,
			  TRUE, G_CALLBACK(remove_cb), data);
    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Remove & Delete",
			  GTK_STOCK_DELETE, TRUE, G_CALLBACK(delete_cb),
			  data);

    gtk_widget_show_all(menu);

    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL,
		   (event != NULL) ? event->button : 0,
		   gdk_event_get_time((GdkEvent *) event));
}

static void
trg_status_icon_view_menu(GtkStatusIcon * icon G_GNUC_UNUSED,
                          GdkEventButton * event,
			  gpointer data)
{
    TrgMainWindowPrivate *priv = TRG_MAIN_WINDOW_GET_PRIVATE(data);
    gboolean connected = priv->client->session != NULL;
    GtkWidget *menu;

    menu = gtk_menu_new();

    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Connect",
			  GTK_STOCK_CONNECT, !connected,
			  G_CALLBACK(connect_cb), data);
    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Disconnect",
			  GTK_STOCK_DISCONNECT, connected,
			  G_CALLBACK(disconnect_cb), data);
    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Add", GTK_STOCK_ADD,
			  connected, G_CALLBACK(add_cb), data);
    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Add from URL",
			  GTK_STOCK_ADD, connected, G_CALLBACK(add_url_cb),
			  data);
    trg_imagemenuitem_new(GTK_MENU_SHELL(menu), "Quit", GTK_STOCK_QUIT,
			  TRUE, G_CALLBACK(quit_cb), data);

    gtk_widget_show_all(menu);

    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL,
		   (event != NULL) ? event->button : 0,
		   gdk_event_get_time((GdkEvent *) event));
}

static gboolean trg_status_icon_popup_menu_cb(GtkStatusIcon * icon,
					      gpointer userdata)
{
    trg_status_icon_view_menu(icon, NULL, userdata);
    return TRUE;
}

static gboolean status_icon_button_press_event(GtkStatusIcon * icon,
					       GdkEventButton * event,
					       gpointer data)
{
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
	trg_status_icon_view_menu(icon, event, data);
	return TRUE;
    } else {
	return FALSE;
    }
}

static gboolean
torrent_tv_button_pressed_cb(GtkWidget * treeview, GdkEventButton * event,
			     gpointer userdata)
{
    GtkTreeSelection *selection;
    GtkTreePath *path;

    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));

	if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview),
					  (gint) event->x,
					  (gint) event->y, &path,
					  NULL, NULL, NULL)) {
	    if (!gtk_tree_selection_path_is_selected(selection, path)) {
		gtk_tree_selection_unselect_all(selection);
		gtk_tree_selection_select_path(selection, path);
	    }

	    gtk_tree_path_free(path);

	    trg_torrent_tv_view_menu(treeview, event, userdata);
	    return TRUE;
	}
    }

    return FALSE;
}

static gboolean torrent_tv_popup_menu_cb(GtkWidget * treeview,
					 gpointer userdata)
{
    trg_torrent_tv_view_menu(treeview, NULL, userdata);
    return TRUE;
}

static GObject *trg_main_window_constructor(GType type,
					    guint
					    n_construct_properties,
					    GObjectConstructParam
					    * construct_params)
{
    TrgMainWindow *self;
    TrgMainWindowPrivate *priv;
    GtkWidget *w;
    GtkWidget *outerVbox;
    GError *iconError = NULL;
    GtkWidget *toolbarHbox;
    gint width, height;

    self = TRG_MAIN_WINDOW(G_OBJECT_CLASS
			   (trg_main_window_parent_class)->constructor
			   (type, n_construct_properties,
			    construct_params));
    priv = TRG_MAIN_WINDOW_GET_PRIVATE(self);

    gtk_window_set_title(GTK_WINDOW(self), PACKAGE_NAME);
    gtk_container_set_border_width(GTK_CONTAINER(self), 5);
    gtk_window_set_default_size(GTK_WINDOW(self), 1000, 600);
    g_signal_connect(G_OBJECT(self), "delete-event",
		     G_CALLBACK(delete_event), NULL);
    g_signal_connect(G_OBJECT(self), "destroy", G_CALLBACK(destroy_window),
		     NULL);

    gtk_window_set_icon_from_file
	(GTK_WINDOW(self), TRG_WINDOW_ICON, &iconError);

    if (iconError != NULL) {
	g_printf("setting icon failed: %s\n", iconError->message);
	g_error_free(iconError);
    }

    priv->torrentModel = trg_torrent_model_new();
    g_signal_connect(priv->torrentModel, "torrent-completed",
		     G_CALLBACK(on_torrent_completed), self);

    priv->filteredTorrentModel =
	gtk_tree_model_filter_new(GTK_TREE_MODEL(priv->torrentModel),
				  NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER
					   (priv->filteredTorrentModel),
					   trg_torrent_tree_view_visible_func,
					   self, NULL);

    priv->sortedTorrentModel =
	gtk_tree_model_sort_new_with_model(priv->filteredTorrentModel);

    priv->torrentTreeView =
	trg_main_window_torrent_tree_view_new(self,
					      priv->sortedTorrentModel,
					      priv->stateSelector);
    g_signal_connect(priv->torrentTreeView, "key-press-event",
		     G_CALLBACK(torrent_tv_key_press_event), self);
    g_signal_connect(priv->torrentTreeView, "popup-menu",
		     G_CALLBACK(torrent_tv_popup_menu_cb), self);
    g_signal_connect(priv->torrentTreeView, "button-press-event",
		     G_CALLBACK(torrent_tv_button_pressed_cb), self);


    outerVbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(self), outerVbox);

    priv->menuBar = trg_main_window_menu_bar_new(self);
    gtk_box_pack_start(GTK_BOX(outerVbox), GTK_WIDGET(priv->menuBar),
		       FALSE, FALSE, 0);

    toolbarHbox = gtk_hbox_new(FALSE, 0);
    priv->toolBar = trg_main_window_toolbar_new(self);
    gtk_box_pack_start(GTK_BOX(toolbarHbox), GTK_WIDGET(priv->toolBar),
		       TRUE, TRUE, 0);

#if GTK_CHECK_VERSION( 2,16,0 )
    w = gtk_entry_new();
    gtk_entry_set_icon_from_stock(GTK_ENTRY(w),
				  GTK_ENTRY_ICON_SECONDARY,
				  GTK_STOCK_CLEAR);
    g_signal_connect(w, "icon-release",
		     G_CALLBACK(clear_filter_entry_cb), w);
    gtk_box_pack_start(GTK_BOX(toolbarHbox), w, FALSE, FALSE, 0);
    g_object_set(w, "secondary-icon-sensitive", FALSE, NULL);
    priv->filterEntryClearButton = priv->filterEntry = w;
#else
    priv->filterEntry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(toolbarHbox), priv->filterEntry, FALSE,
		       FALSE, 0);
    w = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(w), GTK_RELIEF_NONE);
    gtk_button_set_image(GTK_BUTTON(w),
			 gtk_image_new_from_stock(GTK_STOCK_CLEAR,
						  GTK_ICON_SIZE_MENU));
    gtk_box_pack_start(GTK_BOX(toolbarHbox), w, FALSE, FALSE, 0);
    g_signal_connect_swapped(w, "clicked",
			     G_CALLBACK(clear_filter_entry_cb),
			     priv->filterEntry);
    priv->filterEntryClearButton = w;
#endif

    g_signal_connect(G_OBJECT(priv->filterEntry), "changed",
		     G_CALLBACK(entry_filter_changed_cb), self);

    gtk_box_pack_start(GTK_BOX(outerVbox), GTK_WIDGET(toolbarHbox),
		       FALSE, FALSE, 0);

    priv->vpaned = gtk_vpaned_new();
    priv->hpaned = gtk_hpaned_new();
    gtk_box_pack_start(GTK_BOX(outerVbox), priv->vpaned, TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(priv->vpaned), priv->hpaned, TRUE, TRUE);

    priv->stateSelector = trg_state_selector_new();
    gtk_paned_pack1(GTK_PANED(priv->hpaned),
		    GTK_WIDGET(priv->stateSelector), FALSE, FALSE);

    gtk_paned_pack2(GTK_PANED(priv->hpaned),
		    my_scrolledwin_new(GTK_WIDGET
				       (priv->torrentTreeView)), TRUE,
		    TRUE);

    g_signal_connect(G_OBJECT(priv->stateSelector),
		     "torrent-state-changed",
		     G_CALLBACK(torrent_state_selection_changed),
		     priv->filteredTorrentModel);

    priv->notebook = trg_main_window_notebook_new(self);
    gtk_paned_pack2(GTK_PANED(priv->vpaned), priv->notebook, FALSE, FALSE);

    priv->statusIcon = gtk_status_icon_new_from_file(TRG_WINDOW_ICON);
    gtk_status_icon_set_screen(priv->statusIcon,
			       gtk_window_get_screen(GTK_WINDOW(self)));
    g_signal_connect(priv->statusIcon, "activate",
		     G_CALLBACK(status_icon_activated), self);
    g_signal_connect(priv->statusIcon, "button-press-event",
		     G_CALLBACK(status_icon_button_press_event), self);
    g_signal_connect(priv->statusIcon, "popup-menu",
		     G_CALLBACK(trg_status_icon_popup_menu_cb), self);

    priv->statusBar = trg_status_bar_new(priv->statusIcon);
    gtk_box_pack_start(GTK_BOX(outerVbox), GTK_WIDGET(priv->statusBar),
		       FALSE, FALSE, 2);

    width =
	gconf_client_get_int(priv->client->gconf,
			     TRG_GCONF_KEY_WINDOW_WIDTH, NULL);
    height =
	gconf_client_get_int(priv->client->gconf,
			     TRG_GCONF_KEY_WINDOW_HEIGHT, NULL);

    if (width > 0 && height > 0)
	gtk_window_set_default_size(GTK_WINDOW(self), width, height);

    return G_OBJECT(self);
}

static void trg_main_window_class_init(TrgMainWindowClass * klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(TrgMainWindowPrivate));

    object_class->constructor = trg_main_window_constructor;
    object_class->get_property = trg_main_window_get_property;
    object_class->set_property = trg_main_window_set_property;

    g_object_class_install_property(object_class,
				    PROP_CLIENT,
				    g_param_spec_pointer
				    ("trg-client", "TClient",
				     "Client",
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT_ONLY |
				     G_PARAM_STATIC_NAME |
				     G_PARAM_STATIC_NICK |
				     G_PARAM_STATIC_BLURB));
}

void auto_connect_if_required(TrgMainWindow * win, trg_client * tc)
{
    gchar *host =
	gconf_client_get_string(tc->gconf, TRG_GCONF_KEY_HOSTNAME,
				NULL);
    if (host != NULL) {
	gint len = strlen(host);
	g_free(host);
	if (len > 0
	    && gconf_client_get_bool(tc->gconf,
				     TRG_GCONF_KEY_AUTO_CONNECT,
				     NULL) == TRUE)
	    connect_cb(NULL, win);
    }
}

TrgMainWindow *trg_main_window_new(trg_client * tc)
{
    return g_object_new(TRG_TYPE_MAIN_WINDOW, "trg-client", tc, NULL);
}