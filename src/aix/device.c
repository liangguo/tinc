/*
    device.c -- Interaction AIX tun/tap device
    Copyright (C) 2001-2005 Ivo Timmermans,
                  2001-2016 Guus Sliepen <guus@tinc-vpn.org>
                  2009      Grzegorz Dymarek <gregd72002@googlemail.com>
                  2018      Liang Guo <bluestonechina@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "../system.h"

#include "../conf.h"
#include "../device.h"
#include "../logger.h"
#include "../net.h"
#include "../route.h"
#include "../utils.h"
#include "../xalloc.h"

#define DEFAULT_TAP_DEVICE "/dev/tap0"

typedef enum device_type {
	DEVICE_TYPE_TAP,
} device_type_t;

int device_fd = -1;
char *device = NULL;
char *iface = NULL;
static const char *device_info = "AIX tap device";
static uint64_t device_total_in = 0;
static uint64_t device_total_out = 0;

static bool setup_device(void) {
	// Find out which device file to open

	if(!get_config_string(lookup_config(config_tree, "Device"), &device)) {
		if(routing_mode == RMODE_ROUTER) {
			logger(LOG_ERR, "Router mode is not supported on AIX !");
			return false;
		} else {
			device = xstrdup(DEFAULT_TAP_DEVICE);
		}
	}

	// Find out if it's supposed to be a tun or a tap device

	char *type;

	if(get_config_string(lookup_config(config_tree, "DeviceType"), &type)) {
		if(!strcasecmp(type, "tap")) {
			device_type = DEVICE_TYPE_TAP;
		} else {
			logger(LOG_ERR, "Unknown device type %s!", type);
			return false;
		}
	} else {
			if(strstr(device, "tap") || routing_mode != RMODE_ROUTER) {
				device_type = DEVICE_TYPE_TAP;
			}
	}

	if(routing_mode == RMODE_SWITCH && device_type != DEVICE_TYPE_TAP) {
		logger(LOG_ERR, "Only tap devices support switch mode!");
		return false;
	}

	device_fd = open(device, O_RDWR | O_NONBLOCK);
	if(device_fd < 0) {
		logger(LOG_ERR, "Could not open %s: %s", device, strerror(errno));
		return false;
	}

#ifdef FD_CLOEXEC
	fcntl(device_fd, F_SETFD, FD_CLOEXEC);
#endif

	// Guess what the corresponding interface is called

	char *realname = NULL;

#if defined(HAVE_FDEVNAME)
	realname = fdevname(device_fd);
#elif defined(HAVE_DEVNAME)
	struct stat buf;

	if(!fstat(device_fd, &buf)) {
		realname = devname(buf.st_rdev, S_IFCHR);
	}

#endif

	if(!realname) {
		realname = device;
	}

	if(!get_config_string(lookup_config(config_tree, "Interface"), &iface)) {
		iface = xstrdup(strrchr(realname, '/') ? strrchr(realname, '/') + 1 : realname);
	} else if(strcmp(iface, strrchr(realname, '/') ? strrchr(realname, '/') + 1 : realname)) {
		logger(LOG_WARNING, "Warning: Interface does not match Device. $INTERFACE might be set incorrectly.");
	}

	// Configure the device as best as we can

	logger(LOG_INFO, "%s is a %s", device, device_info);
	return true;
}

static void close_device(void) {
	switch(device_type) {

	default:
		close(device_fd);
	}

	free(device);
	free(iface);
}

static bool read_packet(vpn_packet_t *packet) {
	int lenin;

	switch(device_type) {
	case DEVICE_TYPE_TAP:
		if((lenin = read(device_fd, packet->data, MTU)) <= 0) {
			logger(LOG_ERR, "Error while reading from %s %s: %s", device_info,
			       device, strerror(errno));
			return false;
		}

		packet->len = lenin;
		break;

	default:
		return false;
	}

	device_total_in += packet->len;

	ifdebug(TRAFFIC) logger(LOG_DEBUG, "Read packet of %d bytes from %s",
	                        packet->len, device_info);

	return true;
}

static bool write_packet(vpn_packet_t *packet) {
	ifdebug(TRAFFIC) logger(LOG_DEBUG, "Writing packet of %d bytes to %s",
	                        packet->len, device_info);

	switch(device_type) {

	case DEVICE_TYPE_TAP:
		if(write(device_fd, packet->data, packet->len) < 0) {
			logger(LOG_ERR, "Error while writing to %s %s: %s", device_info,
			       device, strerror(errno));
			return false;
		}

		break;

	default:
		return false;
	}

	device_total_out += packet->len;

	return true;
}

static void dump_device_stats(void) {
	logger(LOG_DEBUG, "Statistics for %s %s:", device_info, device);
	logger(LOG_DEBUG, " total bytes in:  %10"PRIu64, device_total_in);
	logger(LOG_DEBUG, " total bytes out: %10"PRIu64, device_total_out);
}

const devops_t os_devops = {
	.setup = setup_device,
	.close = close_device,
	.read = read_packet,
	.write = write_packet,
	.dump_stats = dump_device_stats,
};
