/*
 * Panalyzer.  A Logic Analyzer for the RaspberryPi
 * Copyright (c) 2012 Richard Hirst <richardghirst@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <gtk/gtk.h>
#include "panalyzer.h"

GtkEntry *Status[4];
GtkWidget *DrawingArea;

uint8_t channels[] = DEF_CHANNELS;

struct sigdata_s {
	uint32_t sample;
	uint32_t levels;
};
typedef struct sigdata_s sigdata_t;
typedef struct sigdata_s *sigdata_p;

int num_samples;
sigdata_p sigdata;
uint32_t *tracedata;
int sigcnt;
int zoom_down;
int zooming;
int cursor1;
int cursor2;
int trigger_position = 0;
int buffer_size = 10000;
int run_mode = 0;

panctl_t panctl;
panctl_t prev_panctl = DEF_PANCTL;
panctl_t def_panctl = DEF_PANCTL;

struct view_s {
	int left_margin, right_margin, top, spacing, trace_height, tails;
	int first_sample, last_sample;
	int show_handles;
	GdkRectangle area;
	GdkRectangle handle1;
	GdkRectangle handle2;
};
typedef struct view_s view_t;
typedef view_t *view_p;

view_t preview = {
		20, 10, 6, 8, 5, 4,
		0, 0,
		0,
		{ 0 }
};
view_t mainview = {
		20, 10, 50, 25, 15, 10,
		0, 0,
		1,
		{ 0 }
};

GtkWidget *main_application_window;

/* Surface to store current scribbles */
static cairo_surface_t *surface = NULL;
static cairo_surface_t *preview_cursor = NULL;
static cairo_surface_t *main_cursor = NULL;
static cairo_surface_t *main_cursor_off = NULL;
static int surface_width;

static void error_dialog(const char *fmt, ...);
static void do_draw(GtkWidget *widget);
static void prepopulate_data(void);

static int
sam2pix(view_t *view, int sample)
{
	int samples = view->last_sample - view->first_sample;

	sample -= view->first_sample;

	return (int)(((double)(surface_width - view->left_margin - view->right_margin) * sample / samples) + view->left_margin + 0.5);
}

static int
pix2sam(view_t *view, int pix)
{
	int samples = view->last_sample - view->first_sample;

	pix -= view->left_margin;

	return (int)(((double)(samples) * pix / (surface_width - view->left_margin - view->right_margin)) + view->first_sample + 0.5);
}

static void set_status(int entry, const char *fmt, ...)
{
    va_list ap;
    char str[128];

    va_start(ap, fmt);
    vsnprintf(str, 127, fmt, ap);
    va_end(ap);
    gtk_entry_set_text(Status[entry], str);
}

static void update_delta(void)
{
	int delta = abs(cursor2 - cursor1);

	if (delta > 1000)
		set_status(2, "delta %.3fms", (float)delta/1000);
	else
		set_status(2, "delta %dus", delta);
}

static void
clear_surface (void)
{
  cairo_t *cr;

  cr = cairo_create (surface);

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_paint (cr);

  cairo_destroy (cr);
}

/* Create a new surface of the appropriate size to store our scribbles */
gboolean
configure_event_cb (GtkWidget         *widget,
            GdkEventConfigure *event,
            gpointer           data)
{
  if (surface)
    cairo_surface_destroy (surface);
  if (preview_cursor)
    cairo_surface_destroy(preview_cursor);
  if (main_cursor)
    cairo_surface_destroy(main_cursor);
  if (main_cursor_off)
    cairo_surface_destroy(main_cursor_off);

  surface_width = gtk_widget_get_allocated_width (widget);

  surface = gdk_window_create_similar_surface (gtk_widget_get_window (widget),
                                       CAIRO_CONTENT_COLOR,
                                       gtk_widget_get_allocated_width (widget),
                                       gtk_widget_get_allocated_height (widget));
  preview_cursor = gdk_window_create_similar_surface (gtk_widget_get_window (widget),
                                       CAIRO_CONTENT_COLOR_ALPHA,
                                       1,
                                       sizeof(channels) * preview.spacing + preview.tails);
  main_cursor = gdk_window_create_similar_surface (gtk_widget_get_window (widget),
                                       CAIRO_CONTENT_COLOR_ALPHA,
                                       7,
                                       sizeof(channels) * mainview.spacing + mainview.tails + 7);
  main_cursor_off = gdk_window_create_similar_surface (gtk_widget_get_window (widget),
                                       CAIRO_CONTENT_COLOR_ALPHA,
                                       7, 7);

  cairo_t *cr = cairo_create(preview_cursor);
  cairo_set_line_width(cr, 1);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
  cairo_set_source_rgba(cr, 0.25, 0.25, 1.0, 1.0);
  cairo_move_to(cr, 0.5, 0.5);
  cairo_line_to(cr, 0.5, sizeof(channels) * preview.spacing + preview.tails - 0.5);
  cairo_stroke(cr);
  cairo_destroy(cr);

  mainview.top = sizeof(channels) * preview.spacing + preview.top + 10;
  cr = cairo_create(main_cursor);
  cairo_set_line_width(cr, 1);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
  cairo_set_source_rgba(cr, 0.25, 0.25, 1.0, 1.0);
  cairo_move_to(cr, 3.5, 0.5);
  cairo_line_to(cr, 3.5, sizeof(channels) * mainview.spacing + mainview.tails - 0.5);
  cairo_stroke(cr);
  cairo_rectangle(cr, 0, sizeof(channels) * mainview.spacing + mainview.tails, 7, 7);
  cairo_fill(cr);
  cairo_destroy(cr);

  cr = cairo_create(main_cursor_off);
  cairo_set_source_rgba(cr, 0.25, 0.25, 1.0, 1.0);
  cairo_rectangle(cr, 0, 0, 7, 7);
  cairo_fill(cr);
  cairo_destroy(cr);

  /* Initialize the surface to white */
  clear_surface ();

  do_draw(widget);
  /* We've handled the configure event, no need for further processing. */
  return TRUE;
}

/* Redraw the screen from the surface. Note that the ::draw
 * signal receives a ready-to-be-used cairo_t that is already
 * clipped to only draw the exposed areas of the widget
 */
gboolean
draw_cb (GtkWidget *widget,
 cairo_t   *cr,
 gpointer   data)
{
	double x1,x2,y1,y2;
	cairo_clip_extents(cr,&x1,&y1,&x2,&y2);
	//g_print("draw_cb: %g %g %g %g\n",x1,y1,x2,y2);
	// position 'surface' over the drawing area at 0,0, then paint clipped area of drawing area
  cairo_set_source_surface (cr, surface, 0, 0);
  cairo_paint (cr);
  void do1cursor(cairo_t *cr, int cur) {
  // position 'cursor' over the drawing area at cx,0, the paint the clipped area of drawing area
	  cairo_set_source_surface (cr, preview_cursor, sam2pix(&preview, cur), preview.top);
	  cairo_paint (cr);
    int mx = sam2pix(&mainview, cur);
    if (mx < mainview.left_margin) {
      cairo_set_source_surface (cr, main_cursor_off, mainview.left_margin-3, mainview.top + sizeof(channels) * mainview.spacing + mainview.tails);
    } else if (mx > surface_width - mainview.right_margin) {
      cairo_set_source_surface (cr, main_cursor_off, surface_width - mainview.right_margin - 3, mainview.top + sizeof(channels) * mainview.spacing + mainview.tails);
    } else {
      cairo_set_source_surface (cr, main_cursor, mx-3, mainview.top);
    }
    cairo_paint (cr);
  }
  do1cursor(cr, cursor1);
  do1cursor(cr, cursor2);

  return FALSE;
}

static gint in_rectangle(GdkRectangle *r, GdkEventButton *p) {
	if (p->x < r->x || p->y < r->y || p->x > r->x+r->width || p->y > r->y + r->height)
		return 0;
	else
		return 1;
}

gboolean button_press_event_cb(GtkWidget *widget, GdkEventButton *event,
		gpointer data) {
	/* paranoia check, in case we haven't gotten a configure event */
	if (surface == NULL)
		return FALSE;

	if (event->button == 1) {
		zoom_down = event->x;
		if (in_rectangle(&preview.area, event))
			zooming = 1;
		else if (in_rectangle(&mainview.area, event))
			zooming = 2;
		else if (in_rectangle(&mainview.handle1, event))
			zooming = 3;
		else if (in_rectangle(&mainview.handle2, event))
			zooming = 4;
	}

	return TRUE;
}

gboolean
button_release_event_cb (GtkWidget      *widget,
               GdkEventButton *event,
               gpointer        data)
{
	if (event->button == 1) {
		if (zooming == 1 || zooming == 2) {
			int zoom_up = event->x;
			int width = surface_width - mainview.left_margin - mainview.right_margin;
			int zoom_lo = (zoom_down < zoom_up ? zoom_down : zoom_up) - mainview.left_margin;
			int zoom_hi = (zoom_down > zoom_up ? zoom_down : zoom_up) - mainview.left_margin;
			int handle_up = zoom_up - mainview.left_margin;
			if (handle_up < 0)
				handle_up = 0;
			else if (handle_up > width)
				handle_up = width;
			if (zoom_lo < 0)
				zoom_lo = 0;
			if (zoom_hi >= width)
				zoom_hi = width;
			if (zooming == 1) {
				if (abs(zoom_up - zoom_down) < 2)
					return TRUE;
				mainview.first_sample = (int)((double)zoom_lo * prev_panctl.num_samples / width);
				mainview.last_sample = (int)((double)zoom_hi * prev_panctl.num_samples / width);
			} else if (zooming == 2){
				int offset = mainview.first_sample;
				int samples = mainview.last_sample - mainview.first_sample;
				if (abs(zoom_up - zoom_down) < 2)
					return TRUE;
				mainview.first_sample = (int)((double)zoom_lo * samples / width + offset);
				mainview.last_sample = (int)((double)zoom_hi * samples / width + offset);
			}
			do_draw(widget);
		}
		zooming = 0;
	}

	return TRUE;
}

gboolean
motion_notify_event_cb (GtkWidget      *widget,
                GdkEventMotion *event,
                gpointer        data)
{
	int x, y;
	GdkModifierType state;

  /* paranoia check, in case we haven't gotten a configure event */
  if (surface == NULL)
    return FALSE;

  if (event->is_hint)
		gdk_window_get_pointer(event->window, &x, &y, &state);
	else {
		x = event->x;
		y = event->y;
		state = event->state;
	}

	if (state != GDK_BUTTON1_MASK || (zooming != 3 && zooming != 4))
		return FALSE;

	if (x < mainview.left_margin)
		x = mainview.left_margin;
	else if (x > surface_width - mainview.right_margin)
		x = surface_width - mainview.right_margin;

	void do1cursor(int *cursor, int x, GdkRectangle *rect)
	{
		int opx = sam2pix(&preview, *cursor);
		int omx = sam2pix(&mainview, *cursor);
		*cursor = pix2sam(&mainview, x);
		int npx = sam2pix(&preview, *cursor);
		int nmx = sam2pix(&mainview, *cursor);	// Should = x ...
		if (omx < mainview.left_margin)
			omx = mainview.left_margin;
		else if (omx > surface_width - mainview.right_margin)
			omx = surface_width - mainview.right_margin;

		//g_print("moved to %d (%d,%d,%d)\n", cursor1,omx,x,nmx);
		// gdk_window_process_updates() call prevents the two areas being combined in to one larger one
		// Maybe try XFlush (GDK_DISPLAY ());
		gtk_widget_queue_draw_area (widget, opx, preview.top, 1, sizeof(channels) * preview.spacing + preview.tails);
		gtk_widget_queue_draw_area (widget, npx, preview.top, 1, sizeof(channels) * preview.spacing + preview.tails);
		gdk_window_process_updates(event->window, 1);

		gtk_widget_queue_draw_area (widget, omx-3, mainview.top, 7, sizeof(channels) * mainview.spacing + mainview.tails + 7);
		gtk_widget_queue_draw_area (widget, nmx-3, mainview.top, 7, sizeof(channels) * mainview.spacing + mainview.tails + 7);
		gdk_window_process_updates(event->window, 1);
		rect->x = nmx - 3;
	}
	if (zooming == 3)
		do1cursor(&cursor1, x, &mainview.handle1);
	else
		do1cursor(&cursor2, x, &mainview.handle2);
	update_delta();

	return TRUE;
}

// TODO This is modal, but it doesn't stop the calling code continuing to run... is that a problem?
static void error_dialog(const char *fmt, ...)
{
    GtkWidget *dialog, *label, *content_area;
    va_list ap;
    char str[128];

    va_start(ap, fmt);
    vsnprintf(str, 127, fmt, ap);
    va_end(ap);

   /* Create the widgets */
   dialog = gtk_dialog_new_with_buttons ("Error",
                                         GTK_WINDOW(main_application_window),
                                         GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                         GTK_STOCK_DIALOG_ERROR,
                                         GTK_RESPONSE_NONE,
                                         NULL);
   content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
   label = gtk_label_new (str);

   /* Ensure that the dialog box is destroyed when the user responds */
   g_signal_connect_swapped (dialog,
                             "response",
                             G_CALLBACK (gtk_widget_destroy),
                             dialog);

   /* Add the label, and show everything we've added to the dialog */

   gtk_container_add (GTK_CONTAINER (content_area), label);
   gtk_widget_show_all (dialog);
}

static void prepopulate_data(void)
{
	if (tracedata)
		free(tracedata);
	if (sigdata)
		free(sigdata);
	tracedata = NULL;
	sigdata = NULL;

	memcpy(&panctl, &def_panctl, sizeof(panctl));
	memcpy(&prev_panctl, &def_panctl, sizeof(panctl));
	sigdata = (sigdata_p)malloc(sizeof(sigdata_t)*2);
	sigcnt = 1;
	sigdata[0].sample = 0;
	sigdata[0].levels = 0;
	sigdata[1].sample = panctl.num_samples;
	sigdata[1].levels = 0;
	preview.first_sample = mainview.first_sample = 0;
	preview.last_sample = mainview.last_sample = panctl.num_samples;
	cursor1 = panctl.num_samples/50;
	cursor2 = panctl.num_samples - cursor1;
}

static void do_draw_trigger(GtkWidget *widget, cairo_t *cr, view_p view,
		int position) {
	int visible_samples = view->last_sample - view->first_sample;
	int top = view->top;
	int bot = top + sizeof(channels) * view->spacing;
	int x;

	if (position < view->first_sample || position >= view->last_sample)
		return;

	cairo_set_source_rgb(cr, 1.0, 0.25, 0.25);

	x = (int) ((double) (surface_width - view->left_margin
					- view->right_margin) * (position - view->first_sample)
					/ visible_samples);
	cairo_move_to(cr, x + view->left_margin + 0.5, top + 0.5);
	cairo_line_to(cr, x + view->left_margin + 0.5, bot + view->tails + 0.5);
	cairo_stroke(cr);
	if (view->show_handles) {
		cairo_rectangle(cr, x - 3 + view->left_margin, bot + view->tails, 7, 7);
		cairo_fill(cr);
	}
	cairo_set_source_rgb(cr, 0, 0, 0);
}

static void do_draw1(GtkWidget *widget, cairo_t *cr, view_p view)
{
	int chan;
	int xmin = view->left_margin;
	int xmax = surface_width - view->right_margin;
	int visible_samples = view->last_sample - view->first_sample;
	double xscale = (double)(xmax-xmin)/visible_samples;
	int trigger_samp;

	if (prev_panctl.trigger_point == 0)
		trigger_samp = prev_panctl.num_samples / 20;
	else if (prev_panctl.trigger_point == 1)
		trigger_samp = prev_panctl.num_samples / 2;
	else
		trigger_samp = prev_panctl.num_samples * 19 / 20;

	do_draw_trigger(widget, cr, view, trigger_samp);

	for (chan = 0; chan < sizeof(channels); chan++) {
		int logic0 = view->top + (chan+1) * view->spacing;
		int logic1 = logic0 - view->trace_height;
		int sig;
		int psig = 0;
		int lastx = -1;
		for (sig = 1; sig < sigcnt; sig++) {
			if ((sigdata[psig].levels ^ sigdata[sig].levels) & (1<<channels[chan])) {
				if (sigdata[sig].sample < view->first_sample) {
				}
				else if (sigdata[psig].sample < view->first_sample && sigdata[sig].sample >= view->first_sample) {
					int x1 = xmin;
					int y1 = sigdata[psig].levels & (1<<channels[chan]) ? logic1 : logic0;
					int x2 = (int)(xscale * (sigdata[sig].sample - view->first_sample) + xmin + 0.5);
					if (x2 > xmax)
						x2 = xmax;
					int y2 = y1 == logic0 ? logic1 : logic0;
					cairo_move_to(cr,x1+0.5,y1+0.5);
					cairo_line_to(cr,x2+0.5,y1+0.5);
					if (x2 < xmax) {
						cairo_move_to(cr,x2+0.5,y1+0.5);
						cairo_line_to(cr,x2+0.5,y2+0.5);
					}
				}
				else if (sigdata[sig].sample > view->last_sample) {
					break;
				} else {
					int x1 = (int)(xscale * (sigdata[psig].sample - view->first_sample) + xmin + 0.5);
					int y1 = sigdata[psig].levels & (1<<channels[chan]) ? logic1 : logic0;
					int x2 = (int)(xscale * (sigdata[sig].sample - view->first_sample) + xmin + 0.5);
					int y2 = y1 == logic0 ? logic1 : logic0;
					if (x2 != lastx) {
						cairo_move_to(cr,x1+0.5,y1+0.5);
						cairo_line_to(cr,x2+0.5,y1+0.5);
						cairo_move_to(cr,x2+0.5,y1+0.5);
						cairo_line_to(cr,x2+0.5,y2+0.5);
					}
					lastx = x2;
				}
				psig = sig;
			}
		}
//		g_print("sigcnt %d\n", sigcnt);
		int x1 = (int)(xscale * (sigdata[psig].sample - view->first_sample) + xmin + 0.5);
		int y1 = sigdata[psig].levels & (1<<channels[chan]) ? logic1 : logic0;
		int x2 = (int)(xscale * (sigdata[sig].sample - view->first_sample) + xmin + 0.5);
		if (sigdata[psig].sample > view->last_sample)
			x1 = xmax;
		else if (sigdata[psig].sample < view->first_sample)
			x1 = xmin;
		if (sigdata[sig].sample >= view->last_sample)
			x2 = xmax;
		if (x1 != x2) {
			cairo_move_to(cr,x1+0.5,y1+0.5);
			cairo_line_to(cr,x2+0.5,y1+0.5);
		}
	}
	cairo_stroke(cr);
}

static void do_draw(GtkWidget *widget)
{
  cairo_t *cr;
  double x1,y1,x2,y2;


  clear_surface();
  /* Paint to the surface, where we store our state */
  cr = cairo_create (surface);
  cairo_clip_extents(cr,&x1,&y1,&x2,&y2);
  cairo_set_line_width(cr, 1);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
  
	int c;
	for (c = 0; c < sizeof(channels); c++) {
		char s[4];
		sprintf(s, "%d", c);
		cairo_move_to(cr, 5, mainview.top + mainview.spacing*c+22);
		cairo_show_text(cr, s);
	}

	int zoom1 = (int)((double)(surface_width - preview.left_margin - preview.right_margin) * mainview.first_sample / prev_panctl.num_samples) + preview.left_margin;
	int zoom2 = (int)((double)(surface_width - preview.left_margin - preview.right_margin) * mainview.last_sample / prev_panctl.num_samples) + preview.left_margin + 1;
	cairo_set_source_rgb(cr, 1.0, 0.75, 0.75);
	cairo_rectangle(cr, zoom1, preview.top, zoom2-zoom1, preview.spacing * sizeof(channels) + preview.tails);
	cairo_fill(cr);
	
	preview.area.x = preview.left_margin;
	preview.area.y = preview.top;
	preview.area.width = surface_width - preview.left_margin - preview.right_margin;
	preview.area.height = preview.spacing * sizeof(channels);

	mainview.area.x = mainview.left_margin;
	mainview.area.y = mainview.top;
	mainview.area.width = surface_width - mainview.left_margin - mainview.right_margin;
	mainview.area.height = mainview.spacing * sizeof(channels);

	c = sam2pix(&mainview, cursor1);
	if (c < mainview.left_margin)
		c = mainview.left_margin;
	else if (c > surface_width - mainview.right_margin)
		c = surface_width - mainview.right_margin;
	mainview.handle1.x = c - 3;
	mainview.handle1.y = mainview.top + sizeof(channels) * mainview.spacing + mainview.tails;
	mainview.handle1.width = 7;
	mainview.handle1.height = 7;

	c = sam2pix(&mainview, cursor2);
	if (c < mainview.left_margin)
		c = mainview.left_margin;
	else if (c > surface_width - mainview.right_margin)
		c = surface_width - mainview.right_margin;
	mainview.handle2.x = c - 3;
	mainview.handle2.y = mainview.top + sizeof(channels) * mainview.spacing + mainview.tails;
	mainview.handle2.width = 7;
	mainview.handle2.height = 7;

	int period = (mainview.last_sample - mainview.first_sample);	// in microseconds
	int ideal_steps = (surface_width-20) / 30;
	int best_inc = 1;
	int i = 1;
	int m = 1;
	int n = ideal_steps;
	do {
		n = period / (i * m);
		int diff = abs(n - ideal_steps);
		if (diff < period / best_inc)
			best_inc = i * m;
		if (i == 1)
			i = 2;
		else if (i == 2)
			i = 5;
		else {
			i = 1;
			m *= 10;
		}
	} while (n > ideal_steps / 2);
	int xmin = mainview.left_margin;
	int xmax = surface_width - mainview.right_margin;
	double xscale = (double)(xmax-xmin)/period;
	cairo_set_source_rgb(cr, 1.0, 0.75, 0.75);
	for (i = 0; i < period; i += best_inc) {
		int x = (int)(xscale * i + mainview.left_margin + 0.5);
		cairo_move_to(cr,x+0.5,mainview.top+0.5);
		cairo_line_to(cr,x+0.5,mainview.top+mainview.spacing*sizeof(channels)+mainview.tails+0.5);
	}
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);

	if (best_inc >= 1000)
		set_status(3, "%dms/div", best_inc / 1000);
	else
		set_status(3, "%dus/div", best_inc);
	update_delta();

	do_draw1(widget, cr, &preview);
	do_draw1(widget, cr, &mainview);

//  cairo_path_extents(cr,&x1,&y1,&x2,&y2);
  cairo_stroke(cr);

  cairo_destroy (cr);

  /* Now invalidate the affected region of the drawing area. */
//  g_print("%g %g %g %g\n",x1,y1,x2,y2);
  gtk_widget_queue_draw_area (widget, x1, y1, x2-x1, y2-y1);
}

void do_zoom(GtkWidget *widget, gpointer data) {
	int offset = preview.last_sample * (100 - (int) (long) data) / 2 / 100;
	int num_samples;

	mainview.first_sample = offset;
	mainview.last_sample = preview.last_sample - offset;
	num_samples = mainview.last_sample - mainview.first_sample;
	cursor1 = mainview.first_sample + num_samples / 50;
	cursor2 = mainview.last_sample - num_samples / 50;
	do_draw(DrawingArea);
}

void do_run(GtkWidget *widget, gpointer data) {
	int fd, res;

#if 1
	fd = open("/dev/panalyzer", O_RDWR);
	if (fd >= 0) {
		res = write(fd, &panctl, sizeof(panctl));
		if (res != sizeof(panctl)) {
			error_dialog("Couldn't write device: %s", strerror(errno));
			close(fd);
			return;
		}
	} else {
		error_dialog("Couldn't open device (%s), trying trace.bin", strerror(errno));
#else
	{
#endif
		fd = open("trace.bin", O_RDONLY);
		if (fd < 0) {
			error_dialog("Couldn't open trace.bin: %s", strerror(errno));
			return;
		}
	}

	res = lseek(fd, 0, SEEK_SET);
	if (res < 0) {
		error_dialog("Couldn't seek on data: %s", strerror(errno));
		close(fd);
		return;
	}

	res = read(fd, &panctl, sizeof(panctl));
	if (res < 0) {
		if (run_mode == 0)
			error_dialog("Couldn't read panctl: %s", strerror(errno));
		close(fd);
		return;
	} else 	if (res != sizeof(panctl)) {
		error_dialog("Couldn't read panctl (%d read)", res);
		prepopulate_data();
		close(fd);
		do_draw(widget);
		return;
	}

	if (panctl.magic != PAN_MAGIC) {
		error_dialog("Bad magic in data");
		prepopulate_data();
		close(fd);
		do_draw(widget);
		return;
	}

	if (tracedata)
		free(tracedata);
	if (sigdata)
		free(sigdata);
	tracedata = NULL;
	sigdata = NULL;

	tracedata = (uint32_t *)malloc(panctl.num_samples * sizeof(uint32_t));
	if (tracedata == NULL) {
		error_dialog("Failed to malloc tracedata: %s", strerror(errno));
		gtk_main_quit();
	}

	char *p = (char *)tracedata;
	int siz = panctl.num_samples * sizeof(uint32_t);
	int cnt = siz;
	while (cnt) {
		res = read(fd, p, cnt);
		if (res >= 0) {
			cnt -= res;
			p += res;
		}
		else {
			error_dialog("Failed to read tracedata (read %d of %d): %s",
					siz - cnt, cnt, strerror(errno));
			prepopulate_data();
			close(fd);
			do_draw(widget);
			return;
		}
	}
	close(fd);

	// If we changed the buffer size since the last capture, reset zoom
	// and cursor positions
	if (panctl.num_samples != prev_panctl.num_samples) {
		preview.first_sample = mainview.first_sample = 0;
		preview.last_sample = mainview.last_sample = panctl.num_samples;
		cursor1 = panctl.num_samples/50;
		cursor2 = panctl.num_samples - cursor1;
	}
	memcpy(&prev_panctl, &panctl, sizeof(panctl));

	uint32_t mask = 0;
	int i;
	for (i = 0; i < sizeof(channels); i++)
		mask |= 1 << channels[i];
	// TODO realloc sigdata as necessary
	sigdata = (sigdata_p)malloc(sizeof(sigdata_t)*(panctl.num_samples+1));
	if (sigdata == NULL) {
		error_dialog("Failed to malloc sigdata: %s", strerror(errno));
		gtk_main_quit();
	}
	sigcnt = 0;
	sigdata[sigcnt].sample = 0;
	sigdata[sigcnt].levels = tracedata[0] & mask;
	for (i = 1; i < panctl.num_samples; i++) {
//		tracedata[i] ^= (i&1)<<4;
		if ((tracedata[i] & mask) != sigdata[sigcnt].levels) {
			sigdata[++sigcnt].sample = i;
			sigdata[sigcnt].levels = tracedata[i] & mask;
		}
	}
	sigcnt++;
	sigdata[sigcnt].sample = i;
	sigdata[sigcnt].levels = sigdata[sigcnt-1].levels;
	do_draw(widget);
//	do_analyze();
}

void do_run_mode(GtkWidget *widget, gpointer data) {
	run_mode = (int)(long)data;
}

void do_trigger_position(GtkWidget *widget, gpointer data) {
	panctl.trigger_point = (int)(long)data;
}

void do_buffer_size(GtkWidget *widget, gpointer data) {
	panctl.num_samples = (int)(long)data * 1000;
}


static gboolean continuous_mode_active = FALSE;

// TODO: Is this called in a different thread?  Do we need locks?
static gboolean
time_handler(GtkWidget *widget)
{
	static volatile int processing;
	static volatile int ticker;

	if (processing)
		;
	else if (ticker > 0) {
		ticker--;
	} else {
		processing = 1;
		ticker = 5;
		do_run(DrawingArea, NULL);
		processing = 0;
	}

	return continuous_mode_active;
}

gboolean do_run_button(GtkWidget *widget, GtkWidget *area)
{
	if (continuous_mode_active) {
		continuous_mode_active = FALSE;
		gtk_tool_button_set_stock_id(GTK_TOOL_BUTTON(widget), GTK_STOCK_GO_FORWARD);
	}
	else if (run_mode == 1) {
		continuous_mode_active = TRUE;
		gtk_tool_button_set_stock_id(GTK_TOOL_BUTTON(widget), GTK_STOCK_STOP);
		g_timeout_add(10, (GSourceFunc) time_handler, (gpointer) widget);
	} else {
		do_run(area, NULL);
	}
	return TRUE;
}

static void do_level_select(GtkWidget *widget, gpointer data) {
	const char *levels = "01-";
	char newtxt[2] = { 0 };
	const gchar *txt = gtk_button_get_label(GTK_BUTTON(widget));
	int i = strchr(levels, *txt) - levels;
	if (++i >= strlen(levels))
		i = 0;
	newtxt[0] = levels[i];
	gtk_button_set_label(GTK_BUTTON(widget), newtxt);
}

static void do_trig_enables(GtkWidget *widget, gpointer data) {
	GtkWidget **enables = (GtkWidget **)(data);
	int i;

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
		for (i = MAX_TRIGGERS-1; i >= 0; i--) {
			if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enables[i])))
				break;
		}
		for (--i; i >= 0; i--) {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enables[i]), 1);
		}
	} else {
		for (i = 0; i < MAX_TRIGGERS; i++) {
			if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enables[i])) == 0)
				break;
		}
		for (i++; i < MAX_TRIGGERS; i++) {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enables[i]), 0);
		}
	}
}

// Called when trigger samples changes... ensure value is numeric
static void do_trig_samples(GtkWidget *widget, gpointer data) {
	const char *oldtxt = gtk_entry_get_text(GTK_ENTRY(widget));
	char newtxt[8];
	int i, j;

	for (i = 0, j = 0; i < 7 && oldtxt[i]; i++) {
		if (j == 0 && oldtxt[i] == '0')
			continue;
		if (isdigit(oldtxt[i]))
			newtxt[j++] = oldtxt[i];
	}
	newtxt[j] = '\0';
	gtk_entry_set_text(GTK_ENTRY(widget), newtxt);
}

// Called when trigger samples field loses focus, set to 1 if empty
static gboolean do_trig_samples_focus(GtkWidget *widget, gpointer data) {
	const char *oldtxt = gtk_entry_get_text(GTK_ENTRY(widget));

	if (!*oldtxt)
		gtk_entry_set_text(GTK_ENTRY(widget), "1");

	return FALSE;
}

void do_trigger_dialog(GtkWidget *widget, gpointer data) {
	GtkWidget *dialog, *content_area;
	int i, t;
	GtkWidget *trig_enables[MAX_TRIGGERS];
	GtkWidget *trig_samples[MAX_TRIGGERS];
	GtkWidget *trig_levels[MAX_TRIGGERS][MAX_CHANNELS];

	dialog = gtk_dialog_new_with_buttons("Trigger Conditions", NULL, 0,
			GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
	content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	GtkWidget *grid = gtk_table_new(MAX_TRIGGERS+2, sizeof(channels)+3, FALSE);
	gtk_container_add (GTK_CONTAINER(content_area), grid);
	gtk_table_attach(GTK_TABLE(grid),
			gtk_label_new("Channels"),
			2, sizeof(channels)+2, 0, 1,
			0, 0,
			4, 4);
	for (i = 0; i < sizeof(channels); i++) {
		char txt[10];
		sprintf(txt,"  [%d]  ", i);
		gtk_table_attach(GTK_TABLE(grid),
				gtk_label_new(txt),
				i+2, i+3, 1, 2,
				0, 0,
				4, 4);
	}
	gtk_table_attach(GTK_TABLE(grid),
			gtk_label_new("Enable"),
			1, 2, 1, 2,
			0, 0,
			4, 4);
	gtk_table_attach(GTK_TABLE(grid),
			gtk_label_new("Samples"),
			sizeof(channels)+2, sizeof(channels)+3, 1, 2,
			0, 0,
			4, 4);
	for (t = 0; t < MAX_TRIGGERS; t++) {
		char txt[4];
		sprintf(txt, "%d", t+1);
		gtk_table_attach(GTK_TABLE(grid),
				gtk_label_new(txt),
				0, 1, t+2, t+3,
				0, 0,
				4, 4);
		trig_enables[t] = gtk_check_button_new();
		g_signal_connect(trig_enables[t], "toggled", G_CALLBACK(do_trig_enables), trig_enables);
		gtk_table_attach(GTK_TABLE(grid),
				trig_enables[t],
				1, 2, t+2, t+3,
				0, 0,
				4, 4);
		for (i = 0; i < sizeof(channels); i++) {
			trig_levels[t][i] = gtk_button_new_with_label("-");
			g_signal_connect(trig_levels[t][i], "clicked", G_CALLBACK(do_level_select), NULL);
			gtk_table_attach(GTK_TABLE(grid),
					trig_levels[t][i],
					i+2, i+3, t+2, t+3,
					GTK_FILL|GTK_EXPAND, 0,
					4, 4);
		}
		trig_samples[t] = gtk_entry_new();
		gtk_entry_set_text(GTK_ENTRY(trig_samples[t]), "1");
		gtk_entry_set_max_length(GTK_ENTRY(trig_samples[t]), 6);
		gtk_entry_set_width_chars(GTK_ENTRY(trig_samples[t]), 6);
		gtk_entry_set_alignment(GTK_ENTRY(trig_samples[t]), 1);
		g_signal_connect(trig_samples[t], "changed", G_CALLBACK(do_trig_samples), trig_samples);
		g_signal_connect(trig_samples[t], "focus-out-event", G_CALLBACK(do_trig_samples_focus), trig_samples);
		gtk_table_attach(GTK_TABLE(grid),
				trig_samples[t],
				sizeof(channels)+2, sizeof(channels)+3, t+2, t+3,
				0, 0,
				4, 4);
	}

	// OK, we built the dialog, now populate it with the correct settings
	for (t = 0; t < MAX_TRIGGERS; t++) {
		if (panctl.trigger[t].enabled)
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(trig_enables[t]), 1);
		char txt[8];

		if (panctl.trigger[t].min_samples == 0)
			panctl.trigger[t].min_samples = 1;
		sprintf(txt, "%d", panctl.trigger[t].min_samples);
		gtk_entry_set_text(GTK_ENTRY(trig_samples[t]), txt);
		for (i = 0; i < sizeof(channels); i++) {
			if (panctl.trigger[t].mask & (1 << channels[i])) {
				if (panctl.trigger[t].value & (1 << channels[i]))
					gtk_button_set_label(GTK_BUTTON(trig_levels[t][i]), "1");
				else
					gtk_button_set_label(GTK_BUTTON(trig_levels[t][i]), "0");
			}
		}
	}

	gtk_widget_show_all(dialog);

	// If they hit ok, record new settings
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
		for (t = 0; t < MAX_TRIGGERS; t++) {
			panctl.trigger[t].enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(trig_enables[t]));
			panctl.trigger[t].min_samples = atoi(gtk_entry_get_text(GTK_ENTRY(trig_samples[t])));
			uint32_t mask = 0, value = 0;
			for (i = 0; i < sizeof(channels); i++) {
				const char *txt = gtk_button_get_label(GTK_BUTTON(trig_levels[t][i]));

				if (*txt != '-')
					mask |= 1 << channels[i];
				if (*txt == '1')
					value |= 1 << channels[i];
			}
			panctl.trigger[t].mask = mask;
			panctl.trigger[t].value = value;
		}
	}

	gtk_widget_destroy(dialog);
}

int
main (int   argc,
	  char *argv[])
{
  GtkBuilder *builder;
  GObject *window;
  int i;

  prepopulate_data();

  gtk_init (&argc, &argv);

  /* Construct a GtkBuilder instance and load our UI description */
  builder = gtk_builder_new ();
  gtk_builder_add_from_file (builder, "Panalyzer.ui", NULL);

  /* Connect signal handlers to the constructed widgets. */
  window = gtk_builder_get_object (builder, "MainWindow");
  gtk_builder_connect_signals(builder, NULL);

  for (i = 0; i < 4; i++) {
	  char txt[16];
	  sprintf(txt, "status%d", i);
	  Status[i] = GTK_ENTRY(gtk_builder_get_object (builder, txt));
  }
  DrawingArea = GTK_WIDGET(gtk_builder_get_object(builder, "DrawingArea"));
  // Sadly glade will only let you specify objects as user data in callbacks, so to pass simple values we have to connect them manually
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "run_single_shot_btn")), "activate", G_CALLBACK(do_run_mode), (gpointer)0);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "run_continuous_btn")), "activate", G_CALLBACK(do_run_mode), (gpointer)1);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "zoom100_btn")), "activate", G_CALLBACK(do_zoom), (gpointer)100);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "zoom50_btn")), "activate", G_CALLBACK(do_zoom), (gpointer)50);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "zoom25_btn")), "activate", G_CALLBACK(do_zoom), (gpointer)25);

  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "buf_10ms_btn")), "activate", G_CALLBACK(do_buffer_size), (gpointer)10);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "buf_20ms_btn")), "activate", G_CALLBACK(do_buffer_size), (gpointer)20);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "buf_50ms_btn")), "activate", G_CALLBACK(do_buffer_size), (gpointer)50);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "buf_100ms_btn")), "activate", G_CALLBACK(do_buffer_size), (gpointer)100);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "buf_200ms_btn")), "activate", G_CALLBACK(do_buffer_size), (gpointer)200);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "buf_500ms_btn")), "activate", G_CALLBACK(do_buffer_size), (gpointer)500);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "buf_1000ms_btn")), "activate", G_CALLBACK(do_buffer_size), (gpointer)1000);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "buf_2000ms_btn")), "activate", G_CALLBACK(do_buffer_size), (gpointer)2000);

  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "trig_start_btn")), "activate", G_CALLBACK(do_trigger_position), (gpointer)0);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "trig_centre_btn")), "activate", G_CALLBACK(do_trigger_position), (gpointer)1);
  g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "trig_end_btn")), "activate", G_CALLBACK(do_trigger_position), (gpointer)2);

  gtk_widget_show_all (GTK_WIDGET(window));
  gtk_main ();

  return 0;
}
