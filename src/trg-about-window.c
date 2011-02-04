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


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#include "dispatch.h"
#include "trg-about-window.h"
#include "util.h"

GtkWidget *trg_about_window_new(GtkWindow * parent)
{
    GtkWidget *dialog;
    GdkPixbuf *logo;
    GError *error = NULL;
    const gchar *trgAuthors[] = { "Alan Fitton <ajf@eth0.org.uk>", NULL };

    dialog = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);

    logo =
	gdk_pixbuf_new_from_file("/usr/share/pixmaps/transmission.png",
				 &error);
    if (error == NULL)
	gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), logo);
    else {
	if (error->domain == GDK_PIXBUF_ERROR)
	    g_print("GdkPixbufError: %s\n", error->message);
	else if (error->domain == G_FILE_ERROR)
	    g_print("GFileError: %s\n", error->message);
	else
	    g_print
		("An error in the domain: %d has occurred!\n",
		 error->domain);
	g_error_free(error);
    }

    /* Set application data that will be displayed in the main dialog. */
    gtk_about_dialog_set_name(GTK_ABOUT_DIALOG(dialog), PACKAGE_NAME);
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog),
				 PACKAGE_VERSION);
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog),
				   "(C) 2011 Alan F");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
				  "A remote client to transmission-daemon.");
    /* Set the license text, which is usually loaded from a file. Also, set the
     * web site address and label. */
    gtk_about_dialog_set_license(GTK_ABOUT_DIALOG(dialog), "GPL2");
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
				 "http://code.google.com/p/transmission-remote-gtk/");
    gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
				       "http://code.google.com/p/transmission-remote-gtk/");

    /* Set the application authors, documenters and translators. */
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), trgAuthors);
    /*gtk_about_dialog_set_documenters(GTK_ABOUT_DIALOG(dialog), documenters);
       gtk_about_dialog_set_translator_credits(GTK_ABOUT_DIALOG(dialog),
       "Translator #1\nTranslator #2"); */
    return dialog;
}