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

/*
 * TODO:
 * - Comment the code!
 * - Clean up code
 * - Implement code behind the open/save menu options
 * - Show the equivalent frequency for the period between the cursors
 * - Add panning of main view via middle mouse button
 * - Build triggers dialog once at startup
 * - Optimise screen redraw
 * - Optimise data processing
 * - Add option to select channels
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

/* Backing pixmap for drawing area */
static GdkPixmap *pixmap = NULL;
static GtkWidget *drawing_area;
static GtkWidget *label[6];
static char labeltext[6][32];

static GdkColor zoom_col = { 0, 0xffff, 0xc000, 0xc000 };
static GdkColor handle_col = { 0, 0x4000, 0x4000, 0xffff };
static GdkColor trigger_col = { 0, 0xffff, 0x4000, 0x4000 };

static GdkGC *zoom_gc;
static GdkGC *handle_gc;
static GdkGC *trigger_gc;

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

static void error_dialog(const char *fmt, ...);

static void do_draw_cursor(GtkWidget *widget, view_p view, GdkRectangle *rect, int position, GdkGC *gc)
{
	int width = widget->allocation.width;
	int visible_samples = view->last_sample - view->first_sample;
	int top = view->top;
	int bot = top+sizeof(channels)*view->spacing;
	int x;

	if (position < view->first_sample) {
		if (rect == NULL)
			return;
		x = 0;
	} else if (position >= view->last_sample) {
		if (rect == NULL)
			return;
		x = width - view->left_margin - view->right_margin;
	} else {
		x = (int)((double)(width-view->left_margin-view->right_margin) * (position - view->first_sample) / visible_samples);
		gdk_draw_line(pixmap, gc, x+view->left_margin,top,x+view->left_margin,bot+view->tails);
	}
	if (view->show_handles) {
		gdk_draw_rectangle(pixmap, gc, TRUE, x-3+view->left_margin, bot+view->tails, 7, 7);
		if (rect) {
			rect->x = x-3+view->left_margin;
			rect->y = bot+view->tails;
			rect->width = 7;
			rect->height = 7;
		}
	}
}

static void do_draw1(GtkWidget *widget, view_p view)
{
	int width = widget->allocation.width;
	int chan;
	int xmin = view->left_margin;
	int xmax = width - view->right_margin;
	int visible_samples = view->last_sample - view->first_sample;
	double xscale = (double)(xmax-xmin)/visible_samples;
	int trigger_samp;

	if (prev_panctl.trigger_point == 0)
		trigger_samp = prev_panctl.num_samples / 20;
	else if (prev_panctl.trigger_point == 1)
		trigger_samp = prev_panctl.num_samples / 2;
	else
		trigger_samp = prev_panctl.num_samples * 19 / 20;

	do_draw_cursor(widget, view, NULL, trigger_samp, trigger_gc);
	do_draw_cursor(widget, view, &view->handle1, cursor1, handle_gc);
	do_draw_cursor(widget, view, &view->handle2, cursor2, handle_gc);

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
					gdk_draw_line(pixmap, widget->style->black_gc, x1,y1,x2,y1);
					if (x2 < xmax)
						gdk_draw_line(pixmap, widget->style->black_gc, x2,y1,x2,y2);
				}
				else if (sigdata[sig].sample > view->last_sample) {
					break;
				} else {
					int x1 = (int)(xscale * (sigdata[psig].sample - view->first_sample) + xmin + 0.5);
					int y1 = sigdata[psig].levels & (1<<channels[chan]) ? logic1 : logic0;
					int x2 = (int)(xscale * (sigdata[sig].sample - view->first_sample) + xmin + 0.5);
					int y2 = y1 == logic0 ? logic1 : logic0;
					if (x2 != lastx) {
						gdk_draw_line(pixmap, widget->style->black_gc, x1,y1,x2,y1);
						gdk_draw_line(pixmap, widget->style->black_gc, x2,y1,x2,y2);
					}
					lastx = x2;
				}
				psig = sig;
			}
		}
		int x1 = (int)(xscale * (sigdata[psig].sample - view->first_sample) + xmin + 0.5);
		int y1 = sigdata[psig].levels & (1<<channels[chan]) ? logic1 : logic0;
		int x2 = (int)(xscale * (sigdata[sig].sample - view->first_sample) + xmin + 0.5);
		if (sigdata[psig].sample > view->last_sample)
			x1 = xmax;
		else if (sigdata[psig].sample < view->first_sample)
			x1 = xmin;
		if (sigdata[sig].sample >= view->last_sample)
			x2 = xmax;
		if (x1 != x2)
			gdk_draw_line(pixmap, widget->style->black_gc, x1,y1,x2,y1);
	}
}

static void do_draw(GtkWidget *widget)
{
	int width = widget->allocation.width;
	int height = widget->allocation.height;

	if (pixmap)
		gdk_pixmap_unref(pixmap);

	pixmap = gdk_pixmap_new(widget->window, width, height, -1);
	gdk_draw_rectangle(pixmap, widget->style->white_gc, TRUE, 0, 0,
			width, height);

	zoom_gc = gdk_gc_new(pixmap);
	gdk_gc_set_rgb_fg_color(zoom_gc, &zoom_col);
	handle_gc = gdk_gc_new(pixmap);
	gdk_gc_set_rgb_fg_color(handle_gc, &handle_col);
	trigger_gc = gdk_gc_new(pixmap);
	gdk_gc_set_rgb_fg_color(trigger_gc, &trigger_col);
	// TODO: destroy those GCs when we're done

	PangoLayout *pango = gtk_widget_create_pango_layout(widget, "");
	int c;
	for (c = 0; c < sizeof(channels); c++) {
		char s[4];
		sprintf(s, "%d", c);
		pango_layout_set_text(pango, s, 1);
		gdk_draw_layout(pixmap, widget->style->black_gc, 5, mainview.top + mainview.spacing*c+10, pango);
	}
	g_object_unref(pango);

	if (!sigdata)
		return;
	int zoom1 = (int)((double)(width - preview.left_margin - preview.right_margin) * mainview.first_sample / prev_panctl.num_samples) + preview.left_margin;
	int zoom2 = (int)((double)(width - preview.left_margin - preview.right_margin) * mainview.last_sample / prev_panctl.num_samples) + preview.left_margin + 1;
	gdk_draw_rectangle(pixmap, zoom_gc, TRUE, zoom1, preview.top, zoom2-zoom1, preview.spacing * sizeof(channels) + preview.tails);

	preview.area.x = preview.left_margin;
	preview.area.y = preview.top;
	preview.area.width = width - preview.left_margin - preview.right_margin;
	preview.area.height = preview.spacing * sizeof(channels);

	mainview.area.x = mainview.left_margin;
	mainview.area.y = mainview.top;
	mainview.area.width = width - mainview.left_margin - mainview.right_margin;
	mainview.area.height = mainview.spacing * sizeof(channels);

	int period = (mainview.last_sample - mainview.first_sample);	// in microseconds
	int ideal_steps = (width-20) / 30;
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
	int xmax = width - mainview.right_margin;
	double xscale = (double)(xmax-xmin)/period;
	for (i = 0; i < period; i += best_inc) {
		int x = (int)(xscale * i + mainview.left_margin + 0.5);
		gdk_draw_line(pixmap, zoom_gc, x,mainview.top,x,mainview.top+mainview.spacing*sizeof(channels)+mainview.tails);
	}
	if (best_inc >= 1000)
		sprintf(labeltext[5], "%dms/div", best_inc / 1000);
	else
		sprintf(labeltext[5], "%dus/div", best_inc);
	gtk_label_set_text(GTK_LABEL(label[5]), labeltext[5]);
	int delta = abs(cursor2 - cursor1);
	if (delta > 1000)
		sprintf(labeltext[4], "delta %.3fms", (float)delta/1000);
	else
		sprintf(labeltext[4], "delta %dus", delta);
	gtk_label_set_text(GTK_LABEL(label[4]), labeltext[4]);
	do_draw1(widget, &preview);
	do_draw1(widget, &mainview);
	gtk_widget_queue_draw_area(widget,0,0,width,height);
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

static void do_run(GtkWidget *widget, GtkWidget *area) {
	int fd, res;

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
		error_dialog("Couldn't read panctl: %s", strerror(errno));
		close(fd);
		return;
	} else 	if (res != sizeof(panctl)) {
		error_dialog("Couldn't read panctl (%d read)", res);
		prepopulate_data();
		close(fd);
		do_draw(area);
		return;
	}

	if (panctl.magic != PAN_MAGIC) {
		error_dialog("Bad magic in data");
		prepopulate_data();
		close(fd);
		do_draw(area);
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
			do_draw(area);
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
	sigdata = (sigdata_p)malloc(sizeof(sigdata_t)*panctl.num_samples);
	if (sigdata == NULL) {
		error_dialog("Failed to malloc sigdata: %s", strerror(errno));
		gtk_main_quit();
	}
	sigcnt = 0;
	sigdata[sigcnt].sample = 0;
	sigdata[sigcnt].levels = tracedata[0] & mask;
	for (i = 1; i < panctl.num_samples; i++) {
		if ((tracedata[i] & mask) != sigdata[sigcnt].levels) {
			sigdata[++sigcnt].sample = i;
			sigdata[sigcnt].levels = tracedata[i] & mask;
		}
	}
	sigcnt++;
	sigdata[sigcnt].sample = i;
	sigdata[sigcnt].levels = sigdata[sigcnt-1].levels;
	do_draw(area);
}

static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data) {
	/* If you return FALSE in the "delete-event" signal handler,
	 * GTK will emit the "destroy" signal. Returning TRUE means
	 * you don't want the window to be destroyed.
	 * This is useful for popping up 'are you sure you want to quit?'
	 * type dialogs. */

	/* Change TRUE to FALSE and the main window will be destroyed with
	 * a "delete-event". */

	return FALSE;
}

/* Create a new backing pixmap of the appropriate size */
static gint configure_event(GtkWidget *widget, GdkEventConfigure *event) {
	do_draw(widget);

	return TRUE;
}

/* Redraw the screen from the backing pixmap */
static gint expose_event(GtkWidget *widget, GdkEventExpose *event) {
	gdk_draw_pixmap(widget->window,
			widget->style->fg_gc[GTK_WIDGET_STATE (widget)], pixmap,
			event->area.x, event->area.y, event->area.x, event->area.y,
			event->area.width, event->area.height);

	return FALSE;
}

static gint in_rectangle(GdkRectangle *r, GdkEventButton *p) {
	if (p->x < r->x || p->y < r->y || p->x > r->x+r->width || p->y > r->y + r->height)
		return 0;
	else
		return 1;
}

static gint button_press_event(GtkWidget *widget, GdkEventButton *event) {
	if (event->button == 1 && pixmap != NULL) {
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
//	draw_brush(widget, event->x, event->y);

	return TRUE;
}

static gint button_release_event(GtkWidget *widget, GdkEventButton *event) {
	if (event->button == 1 && pixmap != NULL) {
		if (zooming) {
			int zoom_up = event->x;
			int width = widget->allocation.width - mainview.left_margin - mainview.right_margin;
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
			} else if (zooming == 3) {
				int samples = mainview.last_sample - mainview.first_sample;
				cursor1 = (int)((double)handle_up * samples / width + mainview.first_sample);
			} else if (zooming == 4) {
				int samples = mainview.last_sample - mainview.first_sample;
				cursor2 = (int)((double)handle_up * samples / width + mainview.first_sample);
			}
			zooming = 0;
			do_draw(widget);
		}
	}

	return TRUE;
}

static gint motion_notify_event(GtkWidget *widget, GdkEventMotion *event) {
	int x, y;
	GdkModifierType state;
	int width = widget->allocation.width - mainview.left_margin - mainview.right_margin;
	int handle_up;

	if (event->is_hint)
		gdk_window_get_pointer(event->window, &x, &y, &state);
	else {
		x = event->x;
		y = event->y;
		state = event->state;
	}

	if (state != GDK_BUTTON1_MASK || pixmap == NULL)
		return TRUE;

	handle_up = x - mainview.left_margin;
	if (zooming == 3) {
		int samples = mainview.last_sample - mainview.first_sample;
		cursor1 = (int)((double)handle_up * samples / width + mainview.first_sample);
		do_draw(widget);
	} else if (zooming == 4) {
		int samples = mainview.last_sample - mainview.first_sample;
		cursor2 = (int)((double)handle_up * samples / width + mainview.first_sample);
		do_draw(widget);	// TODO This redraws the whole window.. not very efficient
	}

	return TRUE;
}

/* Another callback */
static void destroy(GtkWidget *widget, gpointer data) {
	gtk_main_quit();
}

static void do_trigger_position(GtkWidget *widget, gpointer data) {
	panctl.trigger_point = (int)(long)data;
}

static void do_buffer_size(GtkWidget *widget, gpointer data) {
	panctl.num_samples = (int)(long)data * 1000;
}

static void do_run_mode(GtkWidget *widget, gpointer data) {
	run_mode = (int)(long)data;
}

static void do_zoom(GtkWidget *widget, gpointer data) {
	int offset = preview.last_sample * (100 - (int)(long)data) / 2 / 100;
	int num_samples;

	mainview.first_sample = offset;
	mainview.last_sample = preview.last_sample - offset;
	num_samples = mainview.last_sample - mainview.first_sample;
	cursor1 = mainview.first_sample + num_samples / 50;
	cursor2 = mainview.last_sample - num_samples / 50;
	do_draw(drawing_area);
}

#if 0
/* Draw a rectangle on the screen */
static void draw_brush(GtkWidget *widget, gdouble x, gdouble y) {
	GdkRectangle update_rect;

	update_rect.x = x - 1;
	update_rect.y = y - 1;
	update_rect.width = 3;
	update_rect.height = 3;
	gdk_draw_rectangle(pixmap, widget->style->black_gc, TRUE, update_rect.x,
			update_rect.y, update_rect.width, update_rect.height);
	gtk_widget_draw(widget, &update_rect);
}
#endif

static void error_dialog(const char *fmt, ...)
{
    GtkWidget *dialog, *label;
    va_list ap;
    char str[128];

    va_start(ap, fmt);
    vsnprintf(str, 127, fmt, ap);
    va_end(ap);
    dialog = gtk_dialog_new_with_buttons("Error", NULL, 0,
                                         GTK_STOCK_OK,
                                         GTK_RESPONSE_OK,
                                         NULL);
    gtk_dialog_set_has_separator(GTK_DIALOG(dialog), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    label = gtk_label_new(str);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(label), 40);
    gtk_container_add(GTK_CONTAINER(GTK_DIALOG(dialog)->vbox), label);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
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

static void do_trigger_dialog(GtkWidget *widget, gpointer data) {
	GtkWidget *dialog;
	int i, t;
	GtkWidget *trig_enables[MAX_TRIGGERS];
	GtkWidget *trig_samples[MAX_TRIGGERS];
	GtkWidget *trig_levels[MAX_TRIGGERS][MAX_CHANNELS];

	dialog = gtk_dialog_new_with_buttons("Trigger Conditions", NULL, 0,
			GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
	GtkWidget *grid = gtk_table_new(MAX_TRIGGERS+2, sizeof(channels)+3, FALSE);
	gtk_box_pack_start (GTK_BOX(GTK_DIALOG(dialog)->vbox), grid, FALSE, TRUE, 10);
	gtk_table_attach(GTK_TABLE(grid),
			gtk_label_new("Channels"),
			2, sizeof(channels)+2, 0, 1,
			0, 0,
			4, 4);
	for (i = 0; i < sizeof(channels); i++) {
		char txt[4];
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

/* This is the GtkItemFactoryEntry structure used to generate new menus.
   Item 1: The menu path. The letter after the underscore indicates an
           accelerator key once the menu is open.
   Item 2: The accelerator key for the entry
   Item 3: The callback function.
   Item 4: The callback action.  This changes the parameters with
           which the function is called.  The default is 0.
   Item 5: The item type, used to define what kind of an item it is.
           Here are the possible values:

           NULL               -> "<Item>"
           ""                 -> "<Item>"
           "<Title>"          -> create a title item
           "<Item>"           -> create a simple item
           "<CheckItem>"      -> create a check item
           "<ToggleItem>"     -> create a toggle item
           "<RadioItem>"      -> create a radio item
           <path>             -> path of a radio item to link against
           "<Separator>"      -> create a separator
           "<Branch>"         -> create an item to hold sub items (optional)
           "<LastBranch>"     -> create a right justified branch
*/

static GtkItemFactoryEntry menu_items[] = {
	{ "/_File",								NULL,			NULL,					0,		"<Branch>"							},
	{ "/File/_Open",						"<control>O",	NULL,					0,		NULL								},
	{ "/File/_Save As",						"<control>S",	NULL,					0,		NULL								},
	{ "/File/sep1",							NULL,			NULL,					0,		"<Separator>"						},
	{ "/File/Quit",							"<control>Q",	gtk_main_quit,			0,		NULL								},
	{ "/_Options",							NULL,			NULL,					0,		"<Branch>"							},
	{ "/Options/Run Mode",					NULL,			NULL,					0,		"<Branch>"							},
	{ "/Options/Run Mode/Single Shot",		NULL,			do_run_mode,			0,		"<RadioItem>"						},
	{ "/Options/Run Mode/Continuous",		NULL,			do_run_mode,			1,		"/Options/Run Mode/Single Shot"		},
	{ "/Options/Trigger Position",			NULL,			NULL,					0,		"<Branch>"							},
	{ "/Options/Trigger Position/Start",	NULL,			do_trigger_position,	0,		"<RadioItem>"						},
	{ "/Options/Trigger Position/Centre",	NULL,			do_trigger_position,	1,		"/Options/Trigger Position/Start"	},
	{ "/Options/Trigger Position/End",		NULL,			do_trigger_position,	2,		"/Options/Trigger Position/Start"	},
	{ "/Options/Trigger Condition...",		"<control>T",	do_trigger_dialog,		0,		NULL								},
	{ "/Options/Buffer Size",				NULL,			NULL,					0,		"<Branch>"							},
	{ "/Options/Buffer Size/10ms",			NULL,			do_buffer_size,			10,		"<RadioItem>"						},
	{ "/Options/Buffer Size/20ms",			NULL,			do_buffer_size,			20,		"/Options/Buffer Size/10ms"			},
	{ "/Options/Buffer Size/50ms",			NULL,			do_buffer_size,			50,		"/Options/Buffer Size/10ms"			},
	{ "/Options/Buffer Size/100ms",			NULL,			do_buffer_size,			100,	"/Options/Buffer Size/10ms"			},
	{ "/Options/Buffer Size/200ms",			NULL,			do_buffer_size,			200,	"/Options/Buffer Size/10ms"			},
	{ "/Options/Buffer Size/500ms",			NULL,			do_buffer_size,			500,	"/Options/Buffer Size/10ms"			},
	{ "/Options/Buffer Size/1000ms",		NULL,			do_buffer_size,			1000,	"/Options/Buffer Size/10ms"			},
	{ "/Options/Buffer Size/2000ms",		NULL,			do_buffer_size,			2000,	"/Options/Buffer Size/10ms"			},
	{ "/Zoom",								NULL,			NULL,					0,		"<Branch>"							},
	{ "/Zoom/100%",							NULL,			do_zoom,				100,	NULL								},
//	{ "/Zoom/50%",							NULL,			do_zoom,				50,		NULL								},
//	{ "/Zoom/20%",							NULL,			do_zoom,				20,		NULL								},
//	{ "/Zoom/10%",							NULL,			do_zoom,				10,		NULL								},
	{ "/_Help",								NULL,			NULL,					0,		"<Branch>"							},
	{ "/_Help/About",						NULL,			NULL,					0,		NULL								},
};


GtkWidget *get_main_menu( GtkWidget  *window)
{
  GtkItemFactory *item_factory;
  GtkAccelGroup *accel_group;
  gint nmenu_items = sizeof (menu_items) / sizeof (menu_items[0]);

  accel_group = gtk_accel_group_new ();

  /* This function initializes the item factory.
     Param 1: The type of menu - can be GTK_TYPE_MENU_BAR, GTK_TYPE_MENU,
              or GTK_TYPE_OPTION_MENU.
     Param 2: The path of the menu.
     Param 3: A pointer to a gtk_accel_group.  The item factory sets up
              the accelerator table while generating menus.
  */

  item_factory = gtk_item_factory_new (GTK_TYPE_MENU_BAR, "<main>",
                                       accel_group);

  /* This function generates the menu items. Pass the item factory,
     the number of items in the array, the array itself, and any
     callback data for the the menu items. */
  gtk_item_factory_create_items (item_factory, nmenu_items, menu_items, NULL);

  /* Attach the new accelerator group to the window. */
  gtk_window_add_accel_group (GTK_WINDOW (window), accel_group);

    /* Finally, return the actual menu bar created by the item factory. */
    return gtk_item_factory_get_widget (item_factory, "<main>");
}

int main(int argc, char *argv[]) {
	/* GtkWidget is the storage type for widgets */
	GtkWidget *window;
	GtkWidget *vbox;
	GtkWidget *button;
	int i;

	prepopulate_data();

	/* This is called in all GTK applications. Arguments are parsed
	 * from the command line and are returned to the application. */
	gtk_init(&argc, &argv);

	/* create a new window */
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	/* When the window is given the "delete-event" signal (this is given
	 * by the window manager, usually by the "close" option, or on the
	 * titlebar), we ask it to call the delete_event () function
	 * as defined above. The data passed to the callback
	 * function is NULL and is ignored in the callback function. */
	g_signal_connect(window, "delete-event", G_CALLBACK(delete_event), NULL);

	/* Here we connect the "destroy" event to a signal handler.
	 * This event occurs when we call gtk_widget_destroy() on the window,
	 * or if we return FALSE in the "delete-event" callback. */
	g_signal_connect(window, "destroy", G_CALLBACK(destroy), NULL);

	/* Sets the border width of the window. */
	gtk_container_set_border_width(GTK_CONTAINER (window), 1);

	vbox = gtk_vbox_new(FALSE, 0);
	gtk_container_add (GTK_CONTAINER (window), vbox);

	/* create the drawing area */
	drawing_area = gtk_drawing_area_new();

	gtk_drawing_area_size(GTK_DRAWING_AREA(drawing_area), 400, 180);

	gtk_signal_connect (GTK_OBJECT (drawing_area), "expose_event",
			(GtkSignalFunc) expose_event, NULL);
	gtk_signal_connect (GTK_OBJECT(drawing_area),"configure_event",
			(GtkSignalFunc) configure_event, NULL);
	gtk_signal_connect (GTK_OBJECT (drawing_area), "motion_notify_event",
			(GtkSignalFunc) motion_notify_event, NULL);
	gtk_signal_connect (GTK_OBJECT (drawing_area), "button_press_event",
			(GtkSignalFunc) button_press_event, NULL);
	gtk_signal_connect (GTK_OBJECT (drawing_area), "button_release_event",
			(GtkSignalFunc) button_release_event, NULL);

	gtk_widget_set_events(drawing_area, GDK_EXPOSURE_MASK
			| GDK_LEAVE_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK
			| GDK_BUTTON_RELEASE_MASK
			| GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK);

	/* Creates a new button with the label "Hello World". */
	button = gtk_button_new_with_label("Run");

	/* When the button receives the "clicked" signal, it will call the
	 * function hello() passing it NULL as its argument.  The hello()
	 * function is defined above. */
	g_signal_connect(button, "clicked", G_CALLBACK(do_run), drawing_area);

	for (i = 0; i < 6; i++)
		label[i] = gtk_label_new("");

	GtkWidget *menubox = gtk_hbox_new(FALSE, 0);
	GtkWidget *menu = get_main_menu(window);
	gtk_box_pack_start (GTK_BOX(vbox), menubox, FALSE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX(menubox), menu, TRUE, TRUE, 0);
	gtk_box_pack_end (GTK_BOX(menubox), button, FALSE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX(vbox), drawing_area, TRUE, TRUE, 0);
	GtkWidget *labelbox = gtk_hbox_new(FALSE, 0);
	gtk_box_pack_start (GTK_BOX(vbox), labelbox, FALSE, TRUE, 0);
	for (i = 0; i < 6; i++)
		gtk_box_pack_start (GTK_BOX(labelbox), label[i], TRUE, TRUE, 0);

	/* The final step is to display everything. */
	gtk_widget_show(menubox);
	gtk_widget_show(menu);
	gtk_widget_show(button);
	gtk_widget_show(drawing_area);
	gtk_widget_show(labelbox);
	for (i = 0; i < 6; i++)
		gtk_widget_show(label[i]);
	gtk_widget_show(vbox);
	gtk_widget_show(window);

	/* All GTK applications must have a gtk_main(). Control ends here
	 * and waits for an event to occur (like a key press or
	 * mouse event). */
	gtk_main();

	return 0;
}

