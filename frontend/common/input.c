/*
 * (C) Gražvydas "notaz" Ignotas, 2008-2010
 *
 * This work is licensed under the terms of any of these licenses
 * (at your option):
 *  - GNU GPL, version 2 or later.
 *  - GNU LGPL, version 2.1 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "plat.h"
#include "lprintf.h"

#ifdef IN_EVDEV
#include "../linux/in_evdev.h"
#endif
#ifdef IN_GP2X
#include "../gp2x/in_gp2x.h"
#endif
#ifdef IN_VK
#include "../win32/in_vk.h"
#endif

typedef struct
{
	int drv_id;
	int drv_fd_hnd;
	void *drv_data;
	char *name;
	int key_count;
	int *binds;	/* total = key_count * bindtypes * 2 */
	const char * const *key_names;
	unsigned int probed:1;
	unsigned int does_combos:1;
} in_dev_t;

static in_drv_t in_drivers[IN_DRVID_COUNT];
static in_dev_t in_devices[IN_MAX_DEVS];
static int in_dev_count = 0;		/* probed + bind devices */
static int in_have_async_devs = 0;
static int menu_key_state = 0;
static int menu_last_used_dev = 0;

#define DRV(id) in_drivers[(unsigned)(id) < IN_DRVID_COUNT ? (id) : 0]


static int *in_alloc_binds(int drv_id, int key_count)
{
	int *binds;

	binds = calloc(key_count * IN_BINDTYPE_COUNT * 2, sizeof(binds[0]));
	if (binds == NULL)
		return NULL;

	DRV(drv_id).get_def_binds(binds + key_count * IN_BINDTYPE_COUNT);
	memcpy(binds, binds + key_count * IN_BINDTYPE_COUNT,
		sizeof(binds[0]) * key_count * IN_BINDTYPE_COUNT);

	return binds;
}

static void in_unprobe(in_dev_t *dev)
{
	if (dev->probed)
		DRV(dev->drv_id).free(dev->drv_data);
	dev->probed = 0;
	dev->drv_data = NULL;
}

static void in_free(in_dev_t *dev)
{
	in_unprobe(dev);
	free(dev->name);
	dev->name = NULL;
	free(dev->binds);
	dev->binds = NULL;
}

/* to be called by drivers
 * async devices must set drv_fd_hnd to -1 */
void in_register(const char *nname, int drv_id, int drv_fd_hnd, void *drv_data,
		int key_count, const char * const *key_names, int combos)
{
	int i, ret, dupe_count = 0, *binds;
	char name[256], *name_end, *tmp;

	strncpy(name, nname, sizeof(name));
	name[sizeof(name)-12] = 0;
	name_end = name + strlen(name);

	for (i = 0; i < in_dev_count; i++)
	{
		if (in_devices[i].name == NULL)
			continue;
		if (strcmp(in_devices[i].name, name) == 0)
		{
			if (in_devices[i].probed) {
				dupe_count++;
				sprintf(name_end, " [%d]", dupe_count);
				continue;
			}
			goto update;
		}
	}

	if (i >= IN_MAX_DEVS)
	{
		/* try to find unused device */
		for (i = 0; i < IN_MAX_DEVS; i++)
			if (!in_devices[i].probed) break;
		if (i >= IN_MAX_DEVS) {
			lprintf("input: too many devices, can't add %s\n", name);
			return;
		}
		in_free(&in_devices[i]);
	}

	tmp = strdup(name);
	if (tmp == NULL)
		return;

	binds = in_alloc_binds(drv_id, key_count);
	if (binds == NULL) {
		free(tmp);
		return;
	}

	in_devices[i].name = tmp;
	in_devices[i].binds = binds;
	in_devices[i].key_count = key_count;
	if (i + 1 > in_dev_count)
		in_dev_count = i + 1;

	lprintf("input: new device #%d \"%s\"\n", i, name);
update:
	in_devices[i].probed = 1;
	in_devices[i].does_combos = combos;
	in_devices[i].drv_id = drv_id;
	in_devices[i].drv_fd_hnd = drv_fd_hnd;
	in_devices[i].key_names = key_names;
	in_devices[i].drv_data = drv_data;

	if (in_devices[i].binds != NULL) {
		ret = DRV(drv_id).clean_binds(drv_data, in_devices[i].binds,
			in_devices[i].binds + key_count * IN_BINDTYPE_COUNT);
		if (ret == 0) {
			/* no useable binds */
			free(in_devices[i].binds);
			in_devices[i].binds = NULL;
		}
	}
}

/* key combo handling, to be called by drivers that support it.
 * Only care about IN_BINDTYPE_EMU */
void in_combos_find(const int *binds, int last_key, int *combo_keys, int *combo_acts)
{
	int act, u;

	*combo_keys = *combo_acts = 0;
	for (act = 0; act < sizeof(binds[0]) * 8; act++)
	{
		int keyc = 0;
		for (u = 0; u <= last_key; u++)
			if (binds[IN_BIND_OFFS(u, IN_BINDTYPE_EMU)] & (1 << act))
				keyc++;

		if (keyc > 1)
		{
			// loop again and mark those keys and actions as combo
			for (u = 0; u <= last_key; u++)
			{
				if (binds[IN_BIND_OFFS(u, IN_BINDTYPE_EMU)] & (1 << act)) {
					*combo_keys |= 1 << u;
					*combo_acts |= 1 << act;
				}
			}
		}
	}
}

int in_combos_do(int keys, const int *binds, int last_key, int combo_keys, int combo_acts)
{
	int i, ret = 0;

	for (i = 0; i <= last_key; i++)
	{
		int acts, acts_c, u;

		if (!(keys & (1 << i)))
			continue;

		acts = binds[IN_BIND_OFFS(i, IN_BINDTYPE_EMU)];
		if (!acts)
			continue;

		if (!(combo_keys & (1 << i))) {
			ret |= acts;
			continue;
		}

		acts_c = acts & combo_acts;
		u = last_key;
		if (acts_c) {
			// let's try to find the other one
			for (u = i + 1; u <= last_key; u++)
				if ( (keys & (1 << u)) && (binds[IN_BIND_OFFS(u, IN_BINDTYPE_EMU)] & acts_c) ) {
					ret |= acts_c & binds[IN_BIND_OFFS(u, IN_BINDTYPE_EMU)];
					keys &= ~((1 << i) | (1 << u));
					break;
				}
		}
		// add non-combo actions if combo ones were not found
		if (u >= last_key)
			ret |= acts & ~combo_acts;
	}

	return ret;
}

void in_probe(void)
{
	int i;

	in_have_async_devs = 0;
	menu_key_state = 0;
	menu_last_used_dev = 0;

	for (i = 0; i < in_dev_count; i++)
		in_unprobe(&in_devices[i]);

	for (i = 1; i < IN_DRVID_COUNT; i++)
		in_drivers[i].probe();

	/* get rid of devs without binds and probes */
	for (i = 0; i < in_dev_count; i++) {
		if (!in_devices[i].probed && in_devices[i].binds == NULL) {
			in_dev_count--;
			if (i < in_dev_count) {
				free(in_devices[i].name);
				memmove(&in_devices[i], &in_devices[i+1],
					(in_dev_count - i) * sizeof(in_devices[0]));
			}

			continue;
		}

		if (in_devices[i].probed && in_devices[i].drv_fd_hnd == -1)
			in_have_async_devs = 1;
	}

	if (in_have_async_devs)
		lprintf("input: async-only devices detected..\n");

	in_debug_dump();
}

/* async update */
int in_update(int *result)
{
	int i, ret = 0;

	for (i = 0; i < in_dev_count; i++) {
		in_dev_t *dev = &in_devices[i];
		if (dev->probed && dev->binds != NULL) {
			// FIXME: this is stupid, make it indirect
			switch (dev->drv_id) {
#ifdef IN_EVDEV
			case IN_DRVID_EVDEV:
				ret |= in_evdev_update(dev->drv_data, dev->binds, result);
				break;
#endif
#ifdef IN_GP2X
			case IN_DRVID_GP2X:
				ret |= in_gp2x_update(dev->drv_data, dev->binds, result);
				break;
#endif
#ifdef IN_VK
			case IN_DRVID_VK:
				ret |= in_vk_update(dev->drv_data, dev->binds, result);
				break;
#endif
			}
		}
	}

	return ret;
}

static int in_update_kc_async(int *dev_id_out, int *is_down_out, int timeout_ms)
{
	int i, is_down, result;
	unsigned int ticks;

	ticks = plat_get_ticks_ms();

	while (1)
	{
		for (i = 0; i < in_dev_count; i++) {
			in_dev_t *d = &in_devices[i];
			if (!d->probed)
				continue;

			result = DRV(d->drv_id).update_keycode(d->drv_data, &is_down);
			if (result == -1)
				continue;

			if (dev_id_out)
				*dev_id_out = i;
			if (is_down_out)
				*is_down_out = is_down;
			return result;
		}

		if (timeout_ms >= 0 && (int)(plat_get_ticks_ms() - ticks) > timeout_ms)
			break;

		plat_sleep_ms(10);
	}

	return -1;
}

/* 
 * wait for a press, always return some keycode or -1 on timeout or error
 */
int in_update_keycode(int *dev_id_out, int *is_down_out, int timeout_ms)
{
	int result = -1, dev_id = 0, is_down, result_menu;
	int fds_hnds[IN_MAX_DEVS];
	int i, ret, count = 0;
	in_drv_t *drv = NULL;
	unsigned int ticks;

	if (in_have_async_devs) {
		result = in_update_kc_async(&dev_id, &is_down, timeout_ms);
		if (result == -1)
			return -1;
		drv = &DRV(in_devices[dev_id].drv_id);
		goto finish;
	}

	ticks = plat_get_ticks_ms();

	for (i = 0; i < in_dev_count; i++) {
		if (in_devices[i].probed)
			fds_hnds[count++] = in_devices[i].drv_fd_hnd;
	}

	if (count == 0) {
		/* don't deadlock, fail */
		lprintf("input: failed to find devices to read\n");
		exit(1);
	}

	while (1)
	{
		ret = plat_wait_event(fds_hnds, count, timeout_ms);
		if (ret < 0)
			break;

		for (i = 0; i < in_dev_count; i++) {
			if (in_devices[i].drv_fd_hnd == ret) {
				dev_id = i;
				break;
			}
		}

		drv = &DRV(in_devices[dev_id].drv_id);
		result = drv->update_keycode(in_devices[dev_id].drv_data, &is_down);
		if (result >= 0)
			break;

		if (result == -2) {
			lprintf("input: \"%s\" errored out, removing.\n", in_devices[dev_id].name);
			in_unprobe(&in_devices[dev_id]);
			break;
		}

		if (timeout_ms >= 0) {
			unsigned int ticks2 = plat_get_ticks_ms();
			timeout_ms -= ticks2 - ticks;
			ticks = ticks2;
			if (timeout_ms <= 0)
				break;
		}
	}

	if (result < 0)
		return -1;
finish:
	/* keep track of menu key state, to allow mixing
	 * in_update_keycode() and in_menu_wait_any() calls */
	result_menu = drv->menu_translate(in_devices[dev_id].drv_data, result);
	if (result_menu != 0) {
		if (is_down)
			menu_key_state |=  result_menu;
		else
			menu_key_state &= ~result_menu;
	}

	if (dev_id_out != NULL)
		*dev_id_out = dev_id;
	if (is_down_out != NULL)
		*is_down_out = is_down;
	return result;
}

/* same as above, only return bitfield of PBTN_*  */
int in_menu_wait_any(int timeout_ms)
{
	int keys_old = menu_key_state;

	while (1)
	{
		int code, is_down = 0, dev_id = 0;

		code = in_update_keycode(&dev_id, &is_down, timeout_ms);
		if (code < 0)
			break;

		if (keys_old != menu_key_state) {
			menu_last_used_dev = dev_id;
			break;
		}
	}

	return menu_key_state;
}

/* wait for menu input, do autorepeat */
int in_menu_wait(int interesting, int autorep_delay_ms)
{
	static int inp_prev = 0;
	static int repeats = 0;
	int ret, release = 0, wait = 450;

	if (repeats)
		wait = autorep_delay_ms;

	ret = in_menu_wait_any(wait);
	if (ret == inp_prev)
		repeats++;

	while (!(ret & interesting)) {
		ret = in_menu_wait_any(-1);
		release = 1;
	}

	if (release || ret != inp_prev)
		repeats = 0;

	inp_prev = ret;

	/* we don't need diagonals in menus */
	if ((ret & PBTN_UP)   && (ret & PBTN_LEFT))  ret &= ~PBTN_LEFT;
	if ((ret & PBTN_UP)   && (ret & PBTN_RIGHT)) ret &= ~PBTN_RIGHT;
	if ((ret & PBTN_DOWN) && (ret & PBTN_LEFT))  ret &= ~PBTN_LEFT;
	if ((ret & PBTN_DOWN) && (ret & PBTN_RIGHT)) ret &= ~PBTN_RIGHT;

	return ret;
}

const int *in_get_dev_binds(int dev_id)
{
	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return NULL;

	return in_devices[dev_id].binds;
}

const int *in_get_dev_def_binds(int dev_id)
{
	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return NULL;

	return in_devices[dev_id].binds + in_devices[dev_id].key_count * IN_BINDTYPE_COUNT;
}

int in_get_config(int dev_id, int what, void *val)
{
	int *ival = val;
	in_dev_t *dev;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS || val == NULL)
		return -1;

	dev = &in_devices[dev_id];
	switch (what) {
	case IN_CFG_BIND_COUNT:
		*ival = dev->key_count;
		break;
	case IN_CFG_DOES_COMBOS:
		*ival = dev->does_combos;
		break;
	case IN_CFG_BLOCKING:
	case IN_CFG_KEY_NAMES:
		return -1; /* not implemented */
	default:
		return DRV(dev->drv_id).get_config(dev->drv_data, what, ival);
	}

	return 0;
}

static int in_set_blocking(int is_blocking)
{
	int i, ret;

	/* have_async_devs means we will have to do all reads async anyway.. */
	if (!in_have_async_devs) {
		for (i = 0; i < in_dev_count; i++) {
			if (in_devices[i].probed)
				DRV(in_devices[i].drv_id).set_config(in_devices[i].drv_data,
								     IN_CFG_BLOCKING, is_blocking);
		}
	}

	menu_key_state = 0;

	/* flush events */
	do {
		ret = in_update_keycode(NULL, NULL, 0);
	} while (ret >= 0);

	return 0;
}

int in_set_config(int dev_id, int what, const void *val, int size)
{
	const int *ival = val;
	in_dev_t *dev;

	if (what == IN_CFG_BLOCKING)
		return in_set_blocking(*ival);

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return -1;

	dev = &in_devices[dev_id];
	if (what == IN_CFG_KEY_NAMES) {
		const char * const *names = val;
		int count = size / sizeof(names[0]);

		if (count < dev->key_count) {
			lprintf("input: set_key_names: not enough keys\n");
			return -1;
		}

		dev->key_names = names;
		return 0;
	}

	return DRV(dev->drv_id).set_config(dev->drv_data, what, *ival);
}

const char *in_get_dev_name(int dev_id, int must_be_active, int skip_pfix)
{
	const char *name, *tmp;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return NULL;

	if (must_be_active && !in_devices[dev_id].probed)
		return NULL;

	name = in_devices[dev_id].name;
	if (name == NULL || !skip_pfix)
		return name;

	/* skip prefix */
	tmp = strchr(name, ':');
	if (tmp != NULL)
		name = tmp + 1;

	return name;
}

int in_name_to_id(const char *dev_name)
{
	int i;

	for (i = 0; i < in_dev_count; i++)
		if (strcmp(dev_name, in_devices[i].name) == 0)
			break;

	if (i >= in_dev_count) {
		lprintf("input: in_name_to_id: no such device: %s\n", dev_name);
		return -1;
	}

	return i;
}

/* never returns NULL */
const char *in_get_key_name(int dev_id, int keycode)
{
	const char *name = NULL;
	static char xname[16];
	in_dev_t *dev;

	if (dev_id < 0)		/* want last used dev? */
		dev_id = menu_last_used_dev;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return "Unkn0";

	dev = &in_devices[dev_id];
	if (keycode < 0)	/* want name for menu key? */
		keycode = DRV(dev->drv_id).menu_translate(dev->drv_data, keycode);

	if (dev->key_names != NULL && 0 <= keycode && keycode < dev->key_count)
		name = dev->key_names[keycode];
	if (name != NULL)
		return name;

	name = DRV(dev->drv_id).get_key_name(keycode);
	if (name != NULL)
		return name;

	/* assume scancode */
	if ((keycode >= '0' && keycode <= '9') || (keycode >= 'a' && keycode <= 'z')
			|| (keycode >= 'A' && keycode <= 'Z'))
		sprintf(xname, "%c", keycode);
	else
		sprintf(xname, "\\x%02X", keycode);
	return xname;
}

int in_get_key_code(int dev_id, const char *key_name)
{
	in_dev_t *dev;
	int i;

	if (dev_id < 0)		/* want last used dev? */
		dev_id = menu_last_used_dev;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return -1;

	dev = &in_devices[dev_id];
	if (dev->key_names == NULL)
		return -1;

	for (i = 0; i < dev->key_count; i++)
		if (dev->key_names[i] && strcasecmp(dev->key_names[i], key_name) == 0)
			return i;

	return -1;
}

int in_bind_key(int dev_id, int keycode, int mask, int bind_type, int force_unbind)
{
	int ret, count;
	in_dev_t *dev;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS || bind_type >= IN_BINDTYPE_COUNT)
		return -1;

	dev = &in_devices[dev_id];
	count = dev->key_count;

	if (dev->binds == NULL) {
		if (force_unbind)
			return 0;
		dev->binds = in_alloc_binds(dev->drv_id, count);
		if (dev->binds == NULL)
			return -1;
	}

	if (keycode < 0 || keycode >= count)
		return -1;
	
	if (force_unbind)
		dev->binds[IN_BIND_OFFS(keycode, bind_type)] &= ~mask;
	else
		dev->binds[IN_BIND_OFFS(keycode, bind_type)] ^=  mask;
	
	ret = DRV(dev->drv_id).clean_binds(dev->drv_data, dev->binds,
				dev->binds + count * IN_BINDTYPE_COUNT);
	if (ret == 0) {
		free(dev->binds);
		dev->binds = NULL;
	}

	return 0;
}

/*
 * Unbind act_mask on binds with type bind_type
 * - if dev_id_ < 0, affects all devices
 *   else only affects dev_id_
 * - if act_mask == -1, unbind all keys
 *   else only actions in mask
 */
void in_unbind_all(int dev_id_, int act_mask, int bind_type)
{
	int dev_id = 0, dev_last = IN_MAX_DEVS - 1;
	int i, count;
	in_dev_t *dev;

	if (dev_id_ >= 0)
		dev_id = dev_last = dev_id_;

	if (bind_type >= IN_BINDTYPE_COUNT)
		return;

	for (; dev_id <= dev_last; dev_id++) {
		dev = &in_devices[dev_id];
		count = dev->key_count;

		if (dev->binds == NULL)
			continue;

		if (act_mask != -1) {
			for (i = 0; i < count; i++)
				dev->binds[IN_BIND_OFFS(i, bind_type)] &= ~act_mask;
		}
		else
			memset(dev->binds, 0, sizeof(dev->binds[0]) * count * IN_BINDTYPE_COUNT);
	}
}

/* returns device id, or -1 on error */
int in_config_parse_dev(const char *name)
{
	int drv_id = -1, i;

	for (i = 0; i < IN_DRVID_COUNT; i++) {
		int len = strlen(in_drivers[i].prefix);
		if (strncmp(name, in_drivers[i].prefix, len) == 0) {
			drv_id = i;
			break;
		}
	}

	if (drv_id < 0) {
		lprintf("input: missing driver for %s\n", name);
		return -1;
	}

	for (i = 0; i < in_dev_count; i++)
	{
		if (in_devices[i].name == NULL)
			continue;
		if (strcmp(in_devices[i].name, name) == 0)
			return i;
	}

	if (i >= IN_MAX_DEVS)
	{
		/* try to find unused device */
		for (i = 0; i < IN_MAX_DEVS; i++)
			if (in_devices[i].name == NULL) break;
		if (i >= IN_MAX_DEVS) {
			lprintf("input: too many devices, can't add %s\n", name);
			return -1;
		}
	}

	memset(&in_devices[i], 0, sizeof(in_devices[i]));

	in_devices[i].name = strdup(name);
	if (in_devices[i].name == NULL)
		return -1;

	in_devices[i].key_names = DRV(drv_id).get_key_names(&in_devices[i].key_count);
	in_devices[i].drv_id = drv_id;

	if (i + 1 > in_dev_count)
		in_dev_count = i + 1;

	return i;
}

int in_config_bind_key(int dev_id, const char *key, int acts, int bind_type)
{
	in_dev_t *dev;
	int i, offs, kc;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS || bind_type >= IN_BINDTYPE_COUNT)
		return -1;
	dev = &in_devices[dev_id];

	/* maybe a raw code? */
	if (key[0] == '\\' && key[1] == 'x') {
		char *p = NULL;
		kc = (int)strtoul(key + 2, &p, 16);
		if (p == NULL || *p != 0)
			kc = -1;
	}
	else {
		/* device specific key name */
		if (dev->binds == NULL) {
			dev->binds = in_alloc_binds(dev->drv_id, dev->key_count);
			if (dev->binds == NULL)
				return -1;
		}

		kc = -1;
		if (dev->key_names != NULL) {
			for (i = 0; i < dev->key_count; i++) {
				const char *k = dev->key_names[i];
				if (k != NULL && strcasecmp(k, key) == 0) {
					kc = i;
					break;
				}
			}
		}

		if (kc < 0)
			kc = DRV(dev->drv_id).get_key_code(key);
		if (kc < 0 && strlen(key) == 1) {
			/* assume scancode */
			kc = key[0];
		}
	}

	if (kc < 0 || kc >= dev->key_count) {
		lprintf("input: bad key: %s\n", key);
		return -1;
	}

	if (bind_type == IN_BINDTYPE_NONE) {
		for (i = 0; i < IN_BINDTYPE_COUNT; i++)
			dev->binds[IN_BIND_OFFS(kc, i)] = 0;
		return 0;
	}

	offs = IN_BIND_OFFS(kc, bind_type);
	if (dev->binds[offs] == -1)
		dev->binds[offs] = 0;
	dev->binds[offs] |= acts;
	return 0;
}

void in_clean_binds(void)
{
	int i;

	for (i = 0; i < IN_MAX_DEVS; i++) {
		int ret, count, *binds, *def_binds;
		in_dev_t *dev = &in_devices[i];

		if (dev->binds == NULL || dev->drv_data == NULL)
			continue;

		count = dev->key_count;
		binds = dev->binds;
		def_binds = binds + count * IN_BINDTYPE_COUNT;

		ret = DRV(dev->drv_id).clean_binds(dev->drv_data, binds, def_binds);
		if (ret == 0) {
			/* no useable binds */
			free(dev->binds);
			dev->binds = NULL;
		}
	}
}

void in_debug_dump(void)
{
	int i;

	lprintf("# drv probed binds name\n");
	for (i = 0; i < IN_MAX_DEVS; i++) {
		in_dev_t *d = &in_devices[i];
		if (!d->probed && d->name == NULL && d->binds == NULL)
			continue;
		lprintf("%d %3d %6c %5c %s\n", i, d->drv_id, d->probed ? 'y' : 'n',
			d->binds ? 'y' : 'n', d->name);
	}
}

/* handlers for unknown/not_preset drivers */

static void in_def_probe(void) {}
static void in_def_free(void *drv_data) {}
static const char * const *
            in_def_get_key_names(int *count) { return NULL; }
static void in_def_get_def_binds(int *binds) {}
static int  in_def_clean_binds(void *drv_data, int *b, int *db) { return 0; }
static int  in_def_get_config(void *drv_data, int what, int *val) { return -1; }
static int  in_def_set_config(void *drv_data, int what, int val) { return -1; }
static int  in_def_update_keycode(void *drv_data, int *is_down) { return 0; }
static int  in_def_menu_translate(void *drv_data, int keycode) { return keycode; }
static int  in_def_get_key_code(const char *key_name) { return -1; }
static const char *in_def_get_key_name(int keycode) { return NULL; }

void in_init(void)
{
	int i;

	memset(in_drivers, 0, sizeof(in_drivers));
	memset(in_devices, 0, sizeof(in_devices));
	in_dev_count = 0;

	for (i = 0; i < IN_DRVID_COUNT; i++) {
		in_drivers[i].prefix = "none:";
		in_drivers[i].probe = in_def_probe;
		in_drivers[i].free = in_def_free;
		in_drivers[i].get_key_names = in_def_get_key_names;
		in_drivers[i].get_def_binds = in_def_get_def_binds;
		in_drivers[i].clean_binds = in_def_clean_binds;
		in_drivers[i].get_config = in_def_get_config;
		in_drivers[i].set_config = in_def_set_config;
		in_drivers[i].update_keycode = in_def_update_keycode;
		in_drivers[i].menu_translate = in_def_menu_translate;
		in_drivers[i].get_key_code = in_def_get_key_code;
		in_drivers[i].get_key_name = in_def_get_key_name;
	}

#ifdef IN_GP2X
	in_gp2x_init(&in_drivers[IN_DRVID_GP2X]);
#endif
#ifdef IN_EVDEV
	in_evdev_init(&in_drivers[IN_DRVID_EVDEV]);
#endif
#ifdef IN_VK
	in_vk_init(&in_drivers[IN_DRVID_VK]);
#endif
}

#if 0
int main(void)
{
	int ret;

	in_init();
	in_probe();

	in_set_blocking(1);

#if 1
	while (1) {
		int dev = 0, down;
		ret = in_update_keycode(&dev, &down);
		lprintf("#%i: %i %i (%s)\n", dev, down, ret, in_get_key_name(dev, ret));
	}
#else
	while (1) {
		ret = in_menu_wait_any();
		lprintf("%08x\n", ret);
	}
#endif

	return 0;
}
#endif
