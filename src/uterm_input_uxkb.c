/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <inttypes.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include "shl_hook.h"
#include "shl_llog.h"
#include "shl_misc.h"
#include "uterm_input.h"
#include "uterm_input_internal.h"

#define LLOG_SUBSYSTEM "uterm_uxkb"

extern const char *_binary_src_uterm_input_fallback_xkb_bin_start;
extern const char *_binary_src_uterm_input_fallback_xkb_bin_end;
extern const size_t _binary_src_uterm_input_fallback_xkb_bin_size;

static void uxkb_log(struct xkb_context *context, enum xkb_log_level level,
		     const char *format, va_list args)
{
	struct uterm_input *input;
	unsigned int sev;

	input = xkb_context_get_user_data(context);
	if (!input->llog)
		return;

	switch (level) {
	case XKB_LOG_LEVEL_CRITICAL:
		sev = LLOG_CRITICAL;
		break;
	case XKB_LOG_LEVEL_ERROR:
		sev = LLOG_ERROR;
		break;
	case XKB_LOG_LEVEL_WARNING:
		sev = LLOG_WARNING;
		break;
	case XKB_LOG_LEVEL_INFO:
		sev = LLOG_INFO;
		break;
	case XKB_LOG_LEVEL_DEBUG:
		/* fallthrough */
	default:
		sev = LLOG_DEBUG;
		break;
	}

	input->llog(input->llog_data,
		    LLOG_DEFAULT,
		    sev,
		    format,
		    args);
}

int uxkb_desc_init(struct uterm_input *input,
		   const char *model,
		   const char *layout,
		   const char *variant,
		   const char *options,
		   const char *locale,
		   const char *keymap,
		   const char *compose_file,
		   size_t compose_file_len)
{
	int ret;
	struct xkb_rule_names rmlvo = {
		.rules = "evdev",
		.model = model,
		.layout = layout,
		.variant = variant,
		.options = options,
	};
	const char *fallback;

	fallback = _binary_src_uterm_input_fallback_xkb_bin_start;

	input->ctx = xkb_context_new(0);
	if (!input->ctx) {
		llog_error(input, "cannot create XKB context");
		return -ENOMEM;
	}

	/* Set logging function. You can use XKB_LOG_VERBOSITY and XKB_LOG_LEVEL
	 * to change the xkbcommon logger. That's why we don't touch the
	 * verbosity and level here. */
	xkb_context_set_user_data(input->ctx, input);
	xkb_context_set_log_fn(input->ctx, uxkb_log);

	/* If a complete keymap file was given, first try that. */
	if (keymap && *keymap) {
		input->keymap = xkb_keymap_new_from_string(input->ctx,
					keymap, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
		if (input->keymap) {
			llog_debug(input,
				   "new keyboard description from memory");
		} else {
			llog_warn(input, "cannot parse keymap, reverting to rmlvo");
		}
	}

	if (!input->keymap) {
		input->keymap = xkb_keymap_new_from_names(input->ctx, &rmlvo, 0);
	}

	if (!input->keymap) {
		llog_warn(input, "failed to create keymap (%s, %s, %s, %s), "
			  "reverting to default system keymap",
			 model, layout, variant, options);

		rmlvo.model = "";
		rmlvo.layout = "";
		rmlvo.variant = "";
		rmlvo.options = "";

		input->keymap = xkb_keymap_new_from_names(input->ctx,
							  &rmlvo, 0);
		if (!input->keymap) {
			llog_warn(input, "failed to create XKB default keymap, "
				  "reverting to built-in fallback");

			input->keymap = xkb_keymap_new_from_string(input->ctx,
					fallback, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
			if (!input->keymap) {
				llog_error(input,
					   "cannot create fallback keymap");
				ret = -EFAULT;
				goto err_ctx;
			}
		}

		llog_debug(input, "new fallback keyboard description");
	} else {
		llog_debug(input, "new keyboard description (%s, %s, %s, %s)",
			   model, layout, variant, options);
	}

	if (compose_file && *compose_file) {
		input->compose_table = xkb_compose_table_new_from_buffer(
					input->ctx,
					compose_file,
					compose_file_len,
					locale,
					XKB_COMPOSE_FORMAT_TEXT_V1,
					0);

		if (input->compose_table) {
			llog_debug(input,
				   "new compose table from memory");
		} else {
			llog_warn(input, "cannot parse compose table, "
				  "reverting to default");
		}
	}

	if (!input->compose_table) {
		input->compose_table = xkb_compose_table_new_from_locale(
								input->ctx,
								locale,
								0);
		if (!input->compose_table) {
			llog_warn(input, "failed to create XKB default compose "
				  "table, disabling compose support");
		}
	}

	return 0;

err_ctx:
	xkb_context_unref(input->ctx);
	return ret;
}

void uxkb_desc_destroy(struct uterm_input *input)
{
	xkb_compose_table_unref(input->compose_table);
	xkb_keymap_unref(input->keymap);
	xkb_context_unref(input->ctx);
}

static void timer_event(struct ev_timer *timer, uint64_t num, void *data)
{
	struct uterm_input_dev *dev = data;

	dev->repeat_event.handled = false;
	shl_hook_call(dev->input->hook, dev->input, &dev->repeat_event);
}

int uxkb_dev_init(struct uterm_input_dev *dev)
{
	int ret;

	ret = ev_eloop_new_timer(dev->input->eloop, &dev->repeat_timer, NULL,
				 timer_event, dev);
	if (ret)
		return ret;

	dev->state = xkb_state_new(dev->input->keymap);
	if (!dev->state) {
		llog_error(dev->input, "cannot create XKB state");
		ret = -ENOMEM;
		goto err_timer;
	}

	if (dev->input->compose_table) {
		dev->compose_state = xkb_compose_state_new(
						dev->input->compose_table,
						0);
		if (!dev->compose_state)
			llog_warn(dev->input, "cannot create compose state, "
				  "disabling compose support");
	}

	return 0;

err_timer:
	ev_eloop_rm_timer(dev->repeat_timer);
	return ret;
}

void uxkb_dev_destroy(struct uterm_input_dev *dev)
{
	xkb_compose_state_unref(dev->compose_state);
	xkb_state_unref(dev->state);
	ev_eloop_rm_timer(dev->repeat_timer);
}

#define EVDEV_KEYCODE_OFFSET 8
enum {
	KEY_RELEASED = 0,
	KEY_PRESSED = 1,
	KEY_REPEATED = 2,
};

static void uxkb_dev_update_keyboard_leds(struct uterm_input_dev *dev)
{
	static const struct {
		int evdev_led;
		const char *xkb_led;
	} leds[] = {
		{ LED_NUML, XKB_LED_NAME_NUM },
		{ LED_CAPSL, XKB_LED_NAME_CAPS },
		{ LED_SCROLLL, XKB_LED_NAME_SCROLL },
	};
	struct input_event events[sizeof(leds) / sizeof(*leds)];
	int i, ret;

	if (!(dev->capabilities & UTERM_DEVICE_HAS_LEDS))
		return;

	memset(events, 0, sizeof(events));

	for (i = 0; i < sizeof(leds) / sizeof(*leds); i++) {
		events[i].type = EV_LED;
		events[i].code = leds[i].evdev_led;
		if (xkb_state_led_name_is_active(dev->state,
						leds[i].xkb_led) > 0)
			events[i].value = 1;
	}

	ret = write(dev->rfd, events, sizeof(events));
	if (ret != sizeof(events))
		llog_warning(dev->input, "cannot update LED state (%d): %m",
			     errno);
}

static inline int uxkb_dev_resize_event(struct uterm_input_dev *dev, size_t s)
{
	uint32_t *tmp;

	if (s > dev->num_syms) {
		tmp = realloc(dev->event.keysyms,
			      sizeof(uint32_t) * s);
		if (!tmp) {
			llog_warning(dev->input,
				     "cannot reallocate keysym buffer");
			return -ENOKEY;
		}
		dev->event.keysyms = tmp;

		tmp = realloc(dev->event.codepoints,
			      sizeof(uint32_t) * s);
		if (!tmp) {
			llog_warning(dev->input,
				     "cannot reallocate codepoints buffer");
			return -ENOKEY;
		}
		dev->event.codepoints = tmp;

		tmp = realloc(dev->repeat_event.keysyms,
			      sizeof(uint32_t) * s);
		if (!tmp) {
			llog_warning(dev->input,
				     "cannot reallocate keysym buffer");
			return -ENOKEY;
		}
		dev->repeat_event.keysyms = tmp;

		tmp = realloc(dev->repeat_event.codepoints,
			      sizeof(uint32_t) * s);
		if (!tmp) {
			llog_warning(dev->input,
				     "cannot reallocate codepoints buffer");
			return -ENOKEY;
		}
		dev->repeat_event.codepoints = tmp;

		dev->num_syms = s;
	}

	return 0;
}

static int uxkb_dev_fill_event(struct uterm_input_dev *dev,
			       struct uterm_input_event *ev,
			       xkb_keycode_t code,
			       int num_syms,
			       const xkb_keysym_t *syms)
{
	int ret, i;

	ret = uxkb_dev_resize_event(dev, num_syms);
	if (ret)
		return ret;

	ev->keycode = code;
	ev->ascii = shl_get_ascii(dev->state, code, syms, num_syms);
	ev->mods = shl_get_xkb_mods(dev->state);
	ev->num_syms = num_syms;
	memcpy(ev->keysyms, syms, sizeof(uint32_t) * num_syms);

	for (i = 0; i < num_syms; ++i) {
		ev->codepoints[i] = xkb_keysym_to_utf32(syms[i]);
		if (!ev->codepoints[i])
			ev->codepoints[i] = UTERM_INPUT_INVALID;
	}

	return 0;
}

static void uxkb_dev_repeat(struct uterm_input_dev *dev, unsigned int state)
{
	struct xkb_keymap *keymap = xkb_state_get_keymap(dev->state);
	unsigned int i;
	int num_keysyms, ret;
	const uint32_t *keysyms;
	struct itimerspec spec;

	if (dev->repeating && dev->repeat_event.keycode == dev->event.keycode) {
		if (state == KEY_RELEASED) {
			dev->repeating = false;
			ev_timer_update(dev->repeat_timer, NULL);
		}
		return;
	}

	if (state == KEY_PRESSED &&
	    xkb_keymap_key_repeats(keymap, dev->event.keycode)) {
		dev->repeat_event.keycode = dev->event.keycode;
		dev->repeat_event.ascii = dev->event.ascii;
		dev->repeat_event.mods = dev->event.mods;
		dev->repeat_event.num_syms = dev->event.num_syms;

		for (i = 0; i < dev->event.num_syms; ++i) {
			dev->repeat_event.keysyms[i] = dev->event.keysyms[i];
			dev->repeat_event.codepoints[i] =
						dev->event.codepoints[i];
		}
	} else if (dev->repeating &&
		   !xkb_keymap_key_repeats(keymap, dev->event.keycode)) {
		num_keysyms = xkb_state_key_get_syms(dev->state,
						     dev->repeat_event.keycode,
						     &keysyms);
		if (num_keysyms <= 0)
			return;

		ret = uxkb_dev_fill_event(dev, &dev->repeat_event,
					  dev->repeat_event.keycode,
					  num_keysyms, keysyms);
		if (ret)
			return;

		return;
	} else {
		return;
	}

	dev->repeating = true;
	spec.it_interval.tv_sec = 0;
	spec.it_interval.tv_nsec = dev->input->repeat_rate * 1000000;
	spec.it_value.tv_sec = 0;
	spec.it_value.tv_nsec = dev->input->repeat_delay * 1000000;
	ev_timer_update(dev->repeat_timer, &spec);
}

int uxkb_dev_process(struct uterm_input_dev *dev,
		     uint16_t key_state, uint16_t code)
{
	struct xkb_state *state;
	struct xkb_compose_state *compose_state;
	xkb_keycode_t keycode;
	const xkb_keysym_t *keysyms;
	xkb_keysym_t one_sym;
	int num_keysyms, ret;
	enum xkb_compose_status compose_status;
	enum xkb_state_component changed;

	if (key_state == KEY_REPEATED)
		return -ENOKEY;

	state = dev->state;
	compose_state = dev->compose_state;
	keycode = code + EVDEV_KEYCODE_OFFSET;

	/*
	 * To summarize the following convoluted logic:
	 *
	 * - single key press may produce one or more keysyms
	 * - if num_keysyms == 1,
	 *     + use get_one_sym to handle the maybe present LOCK mod
	 *     + use the resulting one_sym to feed the compose_state
	 *     + if the keysym completes a compose sequence,
	 *         * compose_state will either produce one keysym, which is set
	 *           back to one_sym, or
	 *         * compose_state will produce NoSymbol, which is treated as a
	 *           cancelled compose sequence
	 *     + if the keysym completes or cancels a sequence, reset
	 *       compose_state
	 * - if num_keysyms != 1,
	 *     + LOCK mod translation doesn't make sense, so skip that
	 *     + compose_state is fed NoSymbol per documentation and the result
	 *       is basically ignored
	 * - update xkb state after querying keysyms per documentation
	 * - if in the process of composing or if the composing was cancelled by
	 *   the key press, stop here
	 * - otherwise process events with keysyms and num_keysyms
	 *
	 * Some background on compose handling:
	 * - https://github.com/xkbcommon/libxkbcommon/issues/4
	 */

	num_keysyms = xkb_state_key_get_syms(state, keycode, &keysyms);

	one_sym = XKB_KEY_NoSymbol;
	if (num_keysyms == 1) {
		/* See: https://bugs.freedesktop.org/show_bug.cgi?id=67167 */
		one_sym = xkb_state_key_get_one_sym(state, keycode);
		keysyms = &one_sym;
	}

	compose_status = XKB_COMPOSE_NOTHING;
	if (compose_state && key_state == KEY_PRESSED) {
		/* XKB_KEY_NoSymbol cancels the current compose sequence. */
		xkb_compose_state_feed(compose_state, one_sym);

		compose_status = xkb_compose_state_get_status(compose_state);

		if (compose_status == XKB_COMPOSE_COMPOSED) {
			one_sym = xkb_compose_state_get_one_sym(compose_state);
			if (one_sym == XKB_KEY_NoSymbol) {
				/*
				 * It is possible that the sequence only
				 * specifies an utf8 string and not a keysym.
				 * Treat this is cancelled, as the rest of the
				 * system can not handle it.
				 */
				compose_status = XKB_COMPOSE_CANCELLED;
			}
		}

		/*
		 * Modifiers are legal key presses, but do not change the
		 * compose_state. If the state is not reset after a sequence is
		 * completed, holding down a modifier after composing repeats
		 * the last composed sequence.
		 */
		if (compose_status == XKB_COMPOSE_COMPOSED ||
		    compose_status == XKB_COMPOSE_CANCELLED)
			xkb_compose_state_reset(compose_state);
	}

	changed = 0;
	if (key_state == KEY_PRESSED)
		changed = xkb_state_update_key(state, keycode, XKB_KEY_DOWN);
	else if (key_state == KEY_RELEASED)
		changed = xkb_state_update_key(state, keycode, XKB_KEY_UP);

	if (changed & XKB_STATE_LEDS)
		uxkb_dev_update_keyboard_leds(dev);

	if (num_keysyms <= 0)
		return -ENOKEY;

	if (compose_status == XKB_COMPOSE_COMPOSING ||
	    compose_status == XKB_COMPOSE_CANCELLED)
		return -ENOKEY;

	ret = uxkb_dev_fill_event(dev, &dev->event, keycode, num_keysyms,
				  keysyms);
	if (ret)
		return -ENOKEY;

	uxkb_dev_repeat(dev, key_state);

	if (key_state == KEY_RELEASED)
		return -ENOKEY;

	dev->event.handled = false;
	shl_hook_call(dev->input->hook, dev->input, &dev->event);

	return 0;
}

void uxkb_dev_sleep(struct uterm_input_dev *dev)
{
	/*
	 * While the device is asleep, we don't receive key events. This
	 * means that when we wake up, the keyboard state may be different
	 * (e.g. some key is pressed but we don't know about it). This can
	 * cause various problems, like stuck modifiers: consider if we
	 * miss a release of the left Shift key. When the user presses it
	 * again, xkb_state_update_key() will think there is *another* left
	 * Shift key that was pressed. When the key is released, it's as if
	 * this "second" key was released, but the "first" is still left
	 * pressed.
	 * To handle this, when the device goes to sleep, we save our
	 * current knowledge of the keyboard's press/release state. On wake
	 * up, we compare the states before and after, and just feed
	 * xkb_state_update_key() the deltas.
	 */
	memset(dev->key_state_bits, 0, sizeof(dev->key_state_bits));
	errno = 0;
	ioctl(dev->rfd, EVIOCGKEY(sizeof(dev->key_state_bits)),
	      dev->key_state_bits);
	if (errno)
		llog_warn(dev->input, "failed to save keyboard state (%d): %m",
			  errno);
}

void uxkb_dev_wake_up(struct uterm_input_dev *dev)
{
	uint32_t code;
	char *old_bits, cur_bits[sizeof(dev->key_state_bits)];
	char old_bit, cur_bit;

	old_bits = dev->key_state_bits;

	memset(cur_bits, 0, sizeof(cur_bits));
	errno = 0;
	ioctl(dev->rfd, EVIOCGKEY(sizeof(cur_bits)), cur_bits);
	if (errno) {
		llog_warn(dev->input,
			  "failed to get current keyboard state (%d): %m",
			  errno);
		return;
	}

	for (code = 0; code < KEY_CNT; code++) {
		old_bit = (old_bits[code / 8] & (1 << (code % 8)));
		cur_bit = (cur_bits[code / 8] & (1 << (code % 8)));

		if (old_bit == cur_bit)
			continue;

		xkb_state_update_key(dev->state, code + EVDEV_KEYCODE_OFFSET,
				     cur_bit ? XKB_KEY_DOWN : XKB_KEY_UP);
	}

	uxkb_dev_update_keyboard_leds(dev);

	if (dev->compose_state)
		xkb_compose_state_reset(dev->compose_state);
}
