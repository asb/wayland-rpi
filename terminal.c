/*
 * Copyright © 2008 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <pty.h>
#include <cairo.h>
#include <glib.h>
#include <linux/input.h>

#include <GL/gl.h>
#include <eagle.h>

#include "wayland-client.h"
#include "wayland-glib.h"

#include "cairo-util.h"
#include "window.h"

static const char gem_device[] = "/dev/dri/card0";
static const char socket_name[] = "\0wayland";

#define MOD_SHIFT	0x01
#define MOD_ALT		0x02
#define MOD_CTRL	0x04

struct terminal {
	struct window *window;
	struct wl_display *display;
	int resize_scheduled;
	char *data;
	int width, height, tail, row, column;
	int fd, master;
	struct buffer *buffer;
	GIOChannel *channel;
	uint32_t modifiers;
};

static void
terminal_draw_contents(struct terminal *terminal)
{
	struct rectangle rectangle;
	cairo_surface_t *surface;
	cairo_t *cr;
	cairo_font_extents_t extents;
	int i, line;

	window_get_child_rectangle(terminal->window, &rectangle);

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
					     rectangle.width, rectangle.height);
	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.9);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(cr, 0, 0.5, 0, 1);

	cairo_select_font_face (cr, "mono",
				CAIRO_FONT_SLANT_NORMAL,
				CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);

	cairo_font_extents(cr, &extents);
	for (i = 0; i < terminal->height; i++) {
		line = (terminal->tail + i) % terminal->height;
		cairo_move_to(cr, 0, extents.ascent + extents.height * i);
		cairo_show_text(cr, &terminal->data[line * (terminal->width + 1)]);
	}
	cairo_destroy(cr);

	if (terminal->buffer != NULL)
		buffer_destroy(terminal->buffer, terminal->fd);

	terminal->buffer = buffer_create_from_cairo_surface(terminal->fd, surface);
	cairo_surface_destroy(surface);

	window_copy(terminal->window,
		    &rectangle,
		    terminal->buffer->name, terminal->buffer->stride);
}

static void
terminal_draw(struct terminal *terminal)
{
	window_draw(terminal->window);
	terminal_draw_contents(terminal);
	wl_display_commit(terminal->display, 0);
}

static gboolean
idle_redraw(void *data)
{
	struct terminal *terminal = data;

	terminal_draw(terminal);

	return FALSE;
}

static void
terminal_data(struct terminal *terminal, const char *data, size_t length)
{
	int i;
	char *row;

	for (i = 0; i < length; i++) {
		row = &terminal->data[terminal->row * (terminal->width + 1)];
		switch (data[i]) {
		case '\r':
			terminal->column = 0;
			break;
		case '\n':
			terminal->row++;
			terminal->column = 0;
			if (terminal->row == terminal->height)
				terminal->row = 0;
			break;
		case '\t':
			memset(&row[terminal->column], ' ', -terminal->column & 7);
			terminal->column = (terminal->column + 7) & ~7;
			break;
		default:
			if (terminal->column < terminal->width)
				row[terminal->column++] = data[i];
			break;
		}
	}
}

static void
resize_handler(struct window *window, int32_t width, int32_t height, void *data)
{
	struct terminal *terminal = data;

	if (!terminal->resize_scheduled) {
		g_idle_add(idle_redraw, terminal);
		terminal->resize_scheduled = 1;
	}
}

static void
acknowledge_handler(struct window *window, uint32_t key, void *data)
{
	struct terminal *terminal = data;

	terminal->resize_scheduled = 0;
}

struct key {
	int code[2];
} evdev_keymap[] = {
	{ { 0, 0 } },		/* 0 */
	{ { 0x1b, 0x1b } },
	{ { '1', '!' } },
	{ { '2', '@' } },
	{ { '3', '#' } },
	{ { '4', '$' } },
	{ { '5', '%' } },
	{ { '6', '^' } },
	{ { '7', '&' } },
	{ { '8', '*' } },
	{ { '9', '(' } },
	{ { '0', ')' } },
	{ { '-', '_' } },
	{ { '=', '+' } },
	{ { '\b', '\b' } },
	{ { '\t', '\t' } },

	{ { 'q', 'Q' } },		/* 16 */
	{ { 'w', 'W' } },
	{ { 'e', 'E' } },
	{ { 'r', 'R' } },
	{ { 't', 'T' } },
	{ { 'y', 'Y' } },
	{ { 'u', 'U' } },
	{ { 'i', 'I' } },
	{ { 'o', 'O' } },
	{ { 'p', 'P' } },
	{ { '[', '{' } },
	{ { ']', '}' } },
	{ { '\n', '\n' } },
	{ { 0, 0 } },
	{ { 'a', 'A' } },
	{ { 's', 'S' } },

	{ { 'd', 'D' } },		/* 32 */
	{ { 'f', 'F' } },
	{ { 'g', 'G' } },
	{ { 'h', 'H' } },
	{ { 'j', 'J' } },
	{ { 'k', 'K' } },
	{ { 'l', 'L' } },
	{ { ';', ':' } },
	{ { '\'', '"' } },
	{ { '`', '~' } },
	{ { 0, 0 } },
	{ { '\\', '|' } },
	{ { 'z', 'Z' } },
	{ { 'x', 'X' } },
	{ { 'c', 'C' } },
	{ { 'v', 'V' } },

	{ { 'b', 'B' } },		/* 48 */
	{ { 'n', 'N' } },
	{ { 'm', 'M' } },
	{ { ',', '<' } },
	{ { '.', '>' } },
	{ { '/', '?' } },
	{ { 0, 0 } },
	{ { '*', '*' } },
	{ { 0, 0 } },
	{ { ' ', ' ' } },
	{ { 0, 0 } }

	/* 59 */
};

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

static void
key_handler(struct window *window, uint32_t key, uint32_t state, void *data)
{
	struct terminal *terminal = data;
	uint32_t mod = 0;
	char c;

	switch (key) {
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
		mod = MOD_SHIFT;
		break;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
		mod = MOD_CTRL;
		break;
	case KEY_LEFTALT:
	case KEY_RIGHTALT:
		mod = MOD_ALT;
		break;
	default:
		if (key < ARRAY_LENGTH(evdev_keymap)) {
			if (terminal->modifiers & MOD_SHIFT)
				c = evdev_keymap[key].code[1];
			else
				c = evdev_keymap[key].code[0];
			if (state && c)
				write(terminal->master, &c, 1);
		}
		break;
	}

	if (state)
		terminal->modifiers |= mod;
	else
		terminal->modifiers &= ~mod;
}

static struct terminal *
terminal_create(struct wl_display *display, int fd)
{
	struct terminal *terminal;
	int size;

	terminal = malloc(sizeof *terminal);
	if (terminal == NULL)
		return terminal;

	memset(terminal, 0, sizeof *terminal);
	terminal->fd = fd;
	terminal->window = window_create(display, fd, "Wayland Terminal",
					 500, 100, 500, 400);
	terminal->display = display;
	terminal->resize_scheduled = 1;
	terminal->width = 80;
	terminal->height = 25;
	size = (terminal->width + 1) * terminal->height;
	terminal->data = malloc(size);
	memset(terminal->data, 0, size);

	window_set_resize_handler(terminal->window, resize_handler, terminal);
	window_set_acknowledge_handler(terminal->window, acknowledge_handler, terminal);
	window_set_key_handler(terminal->window, key_handler, terminal);

	return terminal;
}

static gboolean
io_handler(GIOChannel   *source,
	   GIOCondition  condition,
	   gpointer      data)
{
	struct terminal *terminal = data;
	gchar buffer[256];
	gsize bytes_read;
	GError *error = NULL;

	g_io_channel_read_chars(source, buffer, sizeof buffer,
				&bytes_read, &error);

	terminal_data(terminal, buffer, bytes_read);

	if (!terminal->resize_scheduled) {
		g_idle_add(idle_redraw, terminal);
		terminal->resize_scheduled = 1;
	}

	return TRUE;
}

static int
terminal_run(struct terminal *terminal, const char *path)
{
	int master, slave;
	pid_t pid;

	pid = forkpty(&master, NULL, NULL, NULL);
	if (pid == 0) {
		close(master);
		if (execl(path, path, NULL)) {
			printf("exec failed: %m\n");
			exit(EXIT_FAILURE);
		}
	}

	close(slave);
	terminal->master = master;
	terminal->channel = g_io_channel_unix_new(master);
	fcntl(master, F_SETFL, O_NONBLOCK);
	g_io_add_watch(terminal->channel, G_IO_IN,
		       io_handler, terminal);

	return 0;
}

int main(int argc, char *argv[])
{
	struct wl_display *display;
	int fd;
	GMainLoop *loop;
	GSource *source;
	struct terminal *terminal;

	fd = open(gem_device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "drm open failed: %m\n");
		return -1;
	}

	display = wl_display_create(socket_name, sizeof socket_name);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	loop = g_main_loop_new(NULL, FALSE);
	source = wl_glib_source_new(display);
	g_source_attach(source, NULL);

	terminal = terminal_create(display, fd);
	terminal_run(terminal, "/bin/bash");
	terminal_draw(terminal);

	g_main_loop_run(loop);

	return 0;
}