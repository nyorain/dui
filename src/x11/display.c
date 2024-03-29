#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <errno.h>

#include <unistd.h>
#include <linux/input.h>
#include <sys/poll.h>

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>

#include <pml.h>
#include "shared.h"
#include "display.h"
#include "ui.h"

#define GRAB_KEYBOARD 0

struct display_x11 {
	struct display display;
	struct ui* ui;
	xcb_connection_t* connection;
	xcb_screen_t* screen;
	xcb_ewmh_connection_t ewmh;
	xcb_colormap_t colormap;
	xcb_window_t window;

	xcb_window_t restore_focus;
	uint8_t restore_focus_revert;

	// whether window is using override redirect
	// to make keyboard input work on override redirect windows
	// we probably want to grab the keyboard.
	// But that seems fine for displaying the dashboard.
	// Notifications/banners don't need keyboard input
	bool override_redirect;

	struct {
		xcb_atom_t wm_delete_window;
	} atoms;

	cairo_surface_t* surface;
	cairo_t* cr;

	unsigned width, height;
	enum banner banner; // whether a banner is active.
	bool dashboard; // dashboard currently mapped

	xcb_generic_event_t* pending;
	struct pml_custom* source;
	struct pml_timer* timer;
};

// Simple macro that checks cookies returned by xcb *_checked calls
// useful when something triggers and xcb error
#define XCB_CHECK(x) {\
	xcb_void_cookie_t cookie = (x); \
	xcb_generic_error_t* gerr = xcb_request_check(ctx->connection, cookie); \
	if(gerr) { \
		printf("[%s:%d] xcb error code: %d\n", __FILE__, __LINE__, gerr->error_code); \
		free(gerr); \
	}}

static void configure_window(struct display_x11* dpy, int x, int y,
		unsigned width, unsigned height, bool focus) {
	if(dpy->override_redirect) {
		int32_t values[] = {x, y, width, height};
		uint32_t mask =
			XCB_CONFIG_WINDOW_X |
			XCB_CONFIG_WINDOW_Y |
			XCB_CONFIG_WINDOW_WIDTH |
			XCB_CONFIG_WINDOW_HEIGHT;
		xcb_configure_window(dpy->connection, dpy->window, mask, values);

		xcb_icccm_wm_hints_t wmhints;
		xcb_icccm_wm_hints_set_input(&wmhints, focus);
		xcb_icccm_set_wm_hints(dpy->connection, dpy->window, &wmhints);
	} else {
		xcb_icccm_wm_hints_t wmhints;
		xcb_icccm_wm_hints_set_input(&wmhints, focus);
		xcb_icccm_set_wm_hints(dpy->connection, dpy->window, &wmhints);

		xcb_size_hints_t shints;
		xcb_icccm_size_hints_set_position(&shints, 0, x, y);
		xcb_icccm_size_hints_set_size(&shints, 0, width, height);
		xcb_icccm_set_wm_size_hints(dpy->connection, dpy->window,
			XCB_ATOM_WM_NORMAL_HINTS, &shints);

		xcb_atom_t states[] = {
			dpy->ewmh._NET_WM_STATE_ABOVE,
			dpy->ewmh._NET_WM_STATE_STICKY,
		};
		xcb_ewmh_set_wm_state(&dpy->ewmh, dpy->window,
			sizeof(states) / sizeof(states[0]), states);
	}

	// NOTE: not sure whether it could be problematic to call this here
	// already instead of waiting for the corresponding configure
	// event
	dpy->width = width;
	dpy->height = height;
	cairo_xcb_surface_set_size(dpy->surface, dpy->width, dpy->height);
}

static void draw(struct display_x11* dpy) {
	if(!dpy->dashboard && dpy->banner == banner_none) {
		return;
	}

	// this whole group thing is basically copying
	// the underlying cairo x11 surface doesn't use double buffering
	// so we need it to avoid flickering
	cairo_push_group(dpy->cr);
	ui_draw(dpy->ui, dpy->cr, dpy->width, dpy->height, dpy->banner);

	cairo_pop_group_to_source(dpy->cr);
	cairo_set_operator(dpy->cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(dpy->cr);
	cairo_set_operator(dpy->cr, CAIRO_OPERATOR_OVER);

	cairo_surface_flush(dpy->surface);
}

static void display_map_dashboard(struct display_x11* ctx) {
	if(ctx->dashboard) {
		return;
	}

	// disable banner timer if active
	if(ctx->banner != banner_none) {
		ctx->banner = banner_none;
		pml_timer_disable(ctx->timer);
		xcb_unmap_window(ctx->connection, ctx->window);
	}

	// TODO: don't hardcode center of screen to 1920x1080 res
	// instead use xcb_get_geometry (once, during initialization)
	configure_window(ctx,
		(1920 - start_width) / 2,
		(1080 - start_height) / 2,
		start_width, start_height, true);
	ctx->width = start_width;
	ctx->height = start_height;
	ctx->dashboard = true;

	// initial drawing to avoid undefined contents when first mapped
	draw(ctx);

	const char* title = "dashboard";
	xcb_ewmh_set_wm_name(&ctx->ewmh, ctx->window, strlen(title), title);
	xcb_map_window(ctx->connection, ctx->window);

	if(ctx->override_redirect) {
#if GRAB_KEYBOARD
		bool success = false;

		// try 1000 times to grab keyboard with 1ms timeouts in between (1s)
		// might be needed if a key is currently pressed
		for(int i = 0u; i < 1000; ++i) {
			xcb_grab_keyboard_cookie_t cookie = xcb_grab_keyboard(ctx->connection,
				1, ctx->window, XCB_CURRENT_TIME,
				XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
			xcb_grab_keyboard_reply_t* reply = xcb_grab_keyboard_reply(
				ctx->connection, cookie, NULL);

			if(reply && reply->status == XCB_GRAB_STATUS_SUCCESS) {
				success = true;
				break;
			}
			free(reply);

			struct timespec tsleep = { .tv_nsec = 1000 * 1000 };
			nanosleep(&tsleep, NULL);
		}

		if(!success) {
			printf("Failed to grab keyboard\n");
		}
#else
		xcb_get_input_focus_cookie_t cookie = xcb_get_input_focus(ctx->connection);
		xcb_get_input_focus_reply_t* reply = xcb_get_input_focus_reply(ctx->connection, cookie, NULL);
		if(!reply) {
			printf("Failed to get current input focus\n");
		} else {
			ctx->restore_focus = reply->focus;
			ctx->restore_focus_revert = reply->revert_to;
			free(reply);

			XCB_CHECK(xcb_set_input_focus_checked(ctx->connection,
				XCB_INPUT_FOCUS_POINTER_ROOT, ctx->window, XCB_CURRENT_TIME));
		}
#endif
	}

	xcb_flush(ctx->connection);
}

void display_unmap_dashboard(struct display_x11* ctx) {
	if(!ctx->dashboard) {
		return;
	}

	if(ctx->override_redirect) {
#if GRAB_KEYBOARD
		XCB_CHECK(xcb_ungrab_keyboard_checked(ctx->connection, XCB_CURRENT_TIME));
#else
		assert(ctx->restore_focus);
		XCB_CHECK(xcb_set_input_focus_checked(ctx->connection,
			ctx->restore_focus_revert, ctx->restore_focus,
			XCB_CURRENT_TIME));
		ctx->restore_focus = 0;
#endif
	}

	xcb_unmap_window(ctx->connection, ctx->window);
	ctx->dashboard = false;
	xcb_flush(ctx->connection);
}

static void send_expose_event(struct display_x11* ctx) {
	// NOTE: not exactly sure if xcb is threadsafe and we can use
	// this at any time tbh... there is no XInitThreads for xcb,
	// iirc it is always threadsafe but that should be investigated
	xcb_expose_event_t event = {0};
	event.window = ctx->window;
	event.response_type = XCB_EXPOSE;
	event.x = 0;
	event.y = 0;
	event.width = ctx->width;
	event.height = ctx->height;
	event.count = 1;

	xcb_send_event(ctx->connection, 0, ctx->window, 0, (const char*) &event);
	xcb_flush(ctx->connection);
}

void process(struct display_x11* ctx, xcb_generic_event_t* gev) {
	switch(gev->response_type & 0x7f) {
		// NOTE: could re-enable that but we currently draw the window
		// initially when mapping it to prevent any delay
		// case XCB_MAP_NOTIFY:
		case XCB_EXPOSE:
			draw(ctx);
			break;
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t* ev = (xcb_configure_notify_event_t*) gev;
			if(ev->width != ctx->width || ev->height != ctx->height) {
				cairo_xcb_surface_set_size(ctx->surface, ev->width, ev->height);
				ctx->width = ev->width;
				ctx->height = ev->height;
				printf("resize: %d %d\n", ctx->width, ctx->height);
			}
			break;
		} case XCB_KEY_PRESS: {
			xcb_key_press_event_t* ev = (xcb_key_press_event_t*) gev;
			unsigned keycode = ev->detail - 8;
			if(ctx->dashboard && ui_key(ctx->ui, keycode)) {
				display_unmap_dashboard(ctx);
			} else {
				// TODO: don't always do this. ui should be able to trigger
				// it i guess
				send_expose_event(ctx);
			}
			break;
		} case XCB_CLIENT_MESSAGE: {
			xcb_client_message_event_t* ev = (xcb_client_message_event_t*) gev;
			uint32_t protocol = ev->data.data32[0];
			if(protocol == ctx->atoms.wm_delete_window) {
				display_unmap_dashboard(ctx);
			} else if(protocol == ctx->ewmh._NET_WM_PING) {
				// respond with pong
				xcb_ewmh_send_wm_ping(&ctx->ewmh,
					ctx->screen->root, ev->data.data32[1]);
			}
			break;
		} case 0: {
			xcb_generic_error_t* error = (xcb_generic_error_t*) gev;
			printf("retrieved xcb error code %d\n", error->error_code);
			break;
		} default:
			break;
	}
}


static void banner_timer_cb(struct pml_timer* timer) {
	struct display_x11* ctx = (struct display_x11*) pml_timer_get_data(timer);

	// hide banner
	ctx->banner = banner_none;
	xcb_unmap_window(ctx->connection, ctx->window);
	xcb_flush(ctx->connection);
}

static void toggle_dashboard(struct display* base) {
	struct display_x11* dpy = (struct display_x11*) base;
	if(dpy->dashboard) {
		display_unmap_dashboard(dpy);
	} else {
		display_map_dashboard(dpy);
	}
}

static void show_banner(struct display* base, enum banner banner) {
	struct display_x11* dpy = (struct display_x11*) base;
	// don't show any banners while the dashboard is active
	if(dpy->dashboard) {
		return;
	}

	if(dpy->banner == banner_none) {
		configure_window(dpy,
			1920 - banner_width - banner_margin_x,
			1080 - banner_height - banner_margin_y,
			banner_width, banner_height, false);
		dpy->width = banner_width;
		dpy->height = banner_height;
		dpy->banner = banner;

		// initial draw to avoid undefined contents when mapped
		draw(dpy);

		const char* title = "dui banner";
		xcb_ewmh_set_wm_name(&dpy->ewmh, dpy->window, strlen(title), title);
		xcb_map_window(dpy->connection, dpy->window);
	} else {
		send_expose_event(dpy);
	}

	dpy->banner = banner;

	// set timeout on timer
	// this will automatically override previously queued timers
	struct timespec ts = { .tv_sec = banner_time };
	pml_timer_set_time_rel(dpy->timer, ts);
}

static void destroy(struct display* base) {
	struct display_x11* dpy = (struct display_x11*) base;
	if(dpy->pending) free(dpy->pending);
	if(dpy->timer) pml_timer_destroy(dpy->timer);
	if(dpy->source) pml_custom_destroy(dpy->source);
	if(dpy->cr) cairo_destroy(dpy->cr);
	if(dpy->surface) cairo_surface_destroy(dpy->surface);
	if(dpy->connection) {
		if(dpy->window) {
			xcb_destroy_window(dpy->connection, dpy->window);
		}

		xcb_disconnect(dpy->connection);
	}
}

static void redraw(struct display* base, enum banner banner) {
	struct display_x11* dpy = (struct display_x11*) base;
	if(dpy->dashboard || (dpy->banner != banner_none && dpy->banner == banner)) {
		send_expose_event(dpy);
	}
}

static const struct display_impl x11_impl = {
	.destroy = destroy,
	.toggle_dashboard = toggle_dashboard,
	.redraw = redraw,
	.show_banner = show_banner,
};

// event source
static void es_prepare(struct pml_custom* c) {
	struct display_x11* dpy = (struct display_x11*) pml_custom_get_data(c);
	if(!dpy->pending) {
		// important that there is no reading (i.e. basically any
		// call to xcb, xcb_flush as well) after this.
		// We need this pending mechanism since xcb doesn't tell us
		// whether it has queued events without popping them
		dpy->pending = xcb_poll_for_queued_event(dpy->connection);
	}
}

static unsigned es_query(struct pml_custom* c, struct pollfd* fds,
		unsigned n_fds, int* timeout) {
	struct display_x11* dpy = (struct display_x11*) pml_custom_get_data(c);

	*timeout = dpy->pending ? 0 : -1;
	if(n_fds > 0) {
		fds[0].fd = xcb_get_file_descriptor(dpy->connection),
		fds[0].events = POLLIN;
	}

	return 1;
}

static void es_dispatch(struct pml_custom* c, struct pollfd* fds, unsigned n_fds) {
	(void) fds;
	(void) n_fds;
	struct display_x11* dpy = (struct display_x11*) pml_custom_get_data(c);

	// check for error
	int err = xcb_connection_has_error(dpy->connection);
	if(err != 0) {
		printf("Critical xcb connection error (%d), dui will exit\n", err);
		dui_exit();
		return;
	}

	// dispatch events
	if(dpy->pending) {
		process(dpy, dpy->pending);
		free(dpy->pending);
		dpy->pending = NULL;
	}

	xcb_generic_event_t* gev;
	while((gev = xcb_poll_for_event(dpy->connection))) {
		process(dpy, gev);
		free(gev);
	}

	xcb_flush(dpy->connection);
}

static const struct pml_custom_impl custom_impl = {
	.prepare = es_prepare,
	.query = es_query,
	.dispatch = es_dispatch
};

struct display* display_create_x11(struct ui* ui) {
	struct display_x11* ctx = calloc(1, sizeof(*ctx));
	ctx->display.impl = &x11_impl;
	ctx->ui = ui;
	ctx->override_redirect = true;

	// setup xcb connection
	ctx->connection = xcb_connect(NULL, NULL);
	int err;
	if((err = xcb_connection_has_error(ctx->connection))) {
		printf("xcb connection error: %d\n", err);
		goto err;
	}

	ctx->source = pml_custom_new(dui_pml(), &custom_impl);
	pml_custom_set_data(ctx->source, ctx);

	ctx->screen = xcb_setup_roots_iterator(xcb_get_setup(ctx->connection)).data;

	// load atoms
	// roundtrip only once instead of for every atom
	xcb_intern_atom_cookie_t* ewmh_cookie = xcb_ewmh_init_atoms(ctx->connection, &ctx->ewmh);

	struct {
		const char *name;
		xcb_intern_atom_cookie_t cookie;
		xcb_atom_t *atom;
	} atom[] = {
		{
			.name = "WM_DELETE_WINDOW",
			.atom = &ctx->atoms.wm_delete_window,
		},
	};

	for(size_t i = 0; i < sizeof(atom) / sizeof(atom[0]); ++i) {
		atom[i].cookie = xcb_intern_atom(ctx->connection,
			true, strlen(atom[i].name), atom[i].name);
	}

	for(size_t i = 0; i < sizeof(atom) / sizeof(atom[0]); ++i) {
		xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
			ctx->connection, atom[i].cookie, NULL);

		if(reply) {
			*atom[i].atom = reply->atom;
			free(reply);
		} else {
			*atom[i].atom = XCB_ATOM_NONE;
		}
	}

	xcb_ewmh_init_atoms_replies(&ctx->ewmh, ewmh_cookie, NULL);

	// init visual
	// we want a 32-bit visual
	xcb_depth_iterator_t dit = xcb_screen_allowed_depths_iterator(ctx->screen);

	xcb_visualtype_t* visualtype = NULL;
	unsigned vdepth;
	for(; dit.rem; xcb_depth_next(&dit)) {
		if(dit.data->depth != 32) {
			continue;
		}

		xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(dit.data);
		for(; vit.rem; xcb_visualtype_next(&vit)) {
			visualtype = vit.data;
			vdepth = dit.data->depth;
			break;
		}
	}

	// if no good visual found: just use root visual
	if(!visualtype) {
		printf("Couldn't find 32-bit visual, trying root visual\n");
		for(; dit.rem; xcb_depth_next(&dit)) {
			xcb_visualtype_iterator_t vit = xcb_depth_visuals_iterator(dit.data);
			for(; vit.rem; xcb_visualtype_next(&vit)) {
				if(vit.data->visual_id == ctx->screen->root_visual) {
					visualtype = vit.data;
					vdepth = dit.data->depth;
					break;
				}
			}
		}

		assert(visualtype && "Screen root visual not found");
	}

	// printf("visual: %#010x %#010x %#010x, rgb-bits %d\n",
	// 	visualtype->red_mask,
	// 	visualtype->green_mask,
	// 	visualtype->blue_mask,
	// 	visualtype->bits_per_rgb_value);

	// setup xcb window
	ctx->width = start_width;
	ctx->height = start_height;

	uint32_t mask;

	// colormap: it seems like we have to create a colormap for transparent
	// windows, otherwise they won't display correctly.
	ctx->colormap = xcb_generate_id(ctx->connection);
	XCB_CHECK(xcb_create_colormap_checked(ctx->connection, XCB_COLORMAP_ALLOC_NONE,
		ctx->colormap, ctx->screen->root, visualtype->visual_id));

	// have to specify border pixel (with colormap), otherwise we get bad match
	// not exactly sure why; x11... there may have been a valid reason
	// for this like 30 years ago
	mask = XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
		XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;

  	uint32_t values[4];
	values[0] = 0;
	values[1] = ctx->override_redirect;
	values[2] = XCB_EVENT_MASK_EXPOSURE |
		XCB_EVENT_MASK_KEY_PRESS |
		XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	values[3] = ctx->colormap;

	ctx->window = xcb_generate_id(ctx->connection);
	XCB_CHECK(xcb_create_window_checked(ctx->connection, vdepth, ctx->window,
		ctx->screen->root, 0, 0, ctx->width, ctx->height, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, visualtype->visual_id, mask, values));

	// window properties
	// window type is dialog: floating, over other windows
	xcb_atom_t types[] = {ctx->ewmh._NET_WM_WINDOW_TYPE_DIALOG};
	xcb_ewmh_set_wm_window_type(&ctx->ewmh, ctx->window,
		sizeof(types) / sizeof(types[0]), types);

	xcb_atom_t states[] = {
		ctx->ewmh._NET_WM_STATE_ABOVE,
		ctx->ewmh._NET_WM_STATE_STICKY,
	};
	xcb_ewmh_set_wm_state(&ctx->ewmh, ctx->window,
		sizeof(states) / sizeof(states[0]), states);

	// supported protocols
	// allows to detect when program hangs and kill it
	xcb_atom_t protocols = ctx->ewmh.WM_PROTOCOLS;
	xcb_atom_t supportedProtocols[] = {
		ctx->atoms.wm_delete_window,
		ctx->ewmh._NET_WM_PING
	};

	xcb_change_property(ctx->connection, XCB_PROP_MODE_REPLACE, ctx->window,
		protocols, XCB_ATOM_ATOM, 32, 2, supportedProtocols);

	// pid for ping protocol
	pid_t pid = getpid();
	xcb_ewmh_set_wm_pid(&ctx->ewmh, ctx->window, pid);

	// setup cairo
	ctx->surface = cairo_xcb_surface_create(ctx->connection,
		ctx->window, visualtype, ctx->width, ctx->height);
	ctx->cr = cairo_create(ctx->surface);

	// init timer for banner timeout
	ctx->timer = pml_timer_new(dui_pml(), NULL, banner_timer_cb);
	pml_timer_set_data(ctx->timer, ctx);
	pml_timer_set_clock(ctx->timer, CLOCK_MONOTONIC);
	xcb_flush(ctx->connection);

	return &ctx->display;

err:
	display_destroy(&ctx->display);
	return NULL;
}

