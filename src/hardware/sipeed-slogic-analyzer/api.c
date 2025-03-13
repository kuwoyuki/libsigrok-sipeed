/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2023 taorye <taorye@outlook.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include "protocol.h"

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
};

static const uint32_t drvopts[] = {
	SR_CONF_LOGIC_ANALYZER,
};

static const uint32_t devopts[] = {
	SR_CONF_CONTINUOUS,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_SAMPLERATE    | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_BUFFERSIZE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_TRIGGER_MATCH | SR_CONF_GET | SR_CONF_LIST,
};

enum {
	SLogic_LITE_8 = 0,
	SLogic_BASIC_16_U3,
};

static const struct slogic_model support_models[] = {
	[SLogic_LITE_8] = {
		.name = "Slogic Lite 8",
		.max_samplerate = SR_MHZ(160),
		.max_samplechannel = 8,
		.max_bandwidth = SR_MHZ(320),
	},
	[SLogic_BASIC_16_U3] = {
		.name = "Slogic Basic 16 U3",
		.max_samplerate = SR_MHZ(1600),
		.max_samplechannel = 16,
		.max_bandwidth = SR_MHZ(3200),
	},
};

static const uint64_t samplerates[] = {
	/* 160M = 2^5*5M */
	/* 1600M = 2^6*5^2M */
	SR_MHZ(1),
	SR_MHZ(2),
	SR_MHZ(4),
	SR_MHZ(5),
	SR_MHZ(8),
	SR_MHZ(10),
	SR_MHZ(16),
	SR_MHZ(20),
	SR_MHZ(32),

	/* Slogic Lite 8 */
	/* x 8ch */
	SR_MHZ(40),
	/* x 4ch */
	SR_MHZ(80),
	/* x 2ch */
	SR_MHZ(160),

	/* Slogic Basic 16 U3 */
	/* x 16ch */
	SR_MHZ(200),
	/* x 8ch */
	SR_MHZ(400),
	/* x 4ch */
	SR_MHZ(800),
	/* x 2ch */
	SR_MHZ(1600),
};

static const uint64_t buffersizes[] = {
	2, 4, 8, 16
};

static const int32_t trigger_matches[] = {
	SR_TRIGGER_ZERO,
	SR_TRIGGER_ONE,
	SR_TRIGGER_RISING,
	SR_TRIGGER_FALLING,
	SR_TRIGGER_EDGE,
};

static struct sr_dev_driver sipeed_slogic_analyzer_driver_info;

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	int ret;
	struct sr_dev_inst *sdi;
	struct sr_usb_dev_inst *usb;
	struct drv_context *drvc;
	struct dev_context *devc;

	struct sr_config *option;
	struct libusb_device_descriptor des;
	GSList *devices;
	GSList *l, *conn_devices;
	const char *conn;
	char cbuf[128];
	char *iManufacturer, *iProduct, *iSerialNumber, *iPortPath;

	struct sr_channel *ch;
	unsigned int i;
	gchar *channel_name;

	(void)options;

	conn = NULL;

	devices = NULL;
	drvc = di->context;
	// drvc->instances = NULL;
	
	/* scan for devices, either based on a SR_CONF_CONN option
	 * or on a USB scan. */
	for (l = options; l; l = l->next) {
		option = l->data;
		switch (option->key) {
		case SR_CONF_CONN:
			conn = g_variant_get_string(option->data, NULL);
			sr_info("use conn: %s", conn);
			break;
		default:
			sr_warn("Unhandled option key: %u", option->key);
		}
	}
	
	if(!conn) {
		conn = "359f.3031";
	}

	/* Find all slogic compatible devices. */
	conn_devices = sr_usb_find(drvc->sr_ctx->libusb_ctx, conn);
	for(l = conn_devices; l; l = l->next) {
		usb = l->data;
		ret = sr_usb_open(drvc->sr_ctx->libusb_ctx, usb);
		if (SR_OK != ret) continue;
		libusb_get_device_descriptor(
			libusb_get_device(usb->devhdl), &des);
		libusb_get_string_descriptor_ascii(usb->devhdl,
				des.iManufacturer, cbuf, sizeof(cbuf));
		iManufacturer = g_strdup(cbuf);
		libusb_get_string_descriptor_ascii(usb->devhdl,
				des.iProduct, cbuf, sizeof(cbuf));
		iProduct = g_strdup(cbuf);
		libusb_get_string_descriptor_ascii(usb->devhdl,
				des.iSerialNumber, cbuf, sizeof(cbuf));
		iSerialNumber = g_strdup(cbuf);
		usb_get_port_path(libusb_get_device(usb->devhdl),
				cbuf, sizeof(cbuf));
		iPortPath = g_strdup(cbuf);
		sr_usb_close(usb);

		sdi = sr_dev_inst_user_new(iManufacturer, iProduct, NULL);
		sdi->serial_num = iSerialNumber;
		sdi->connection_id = iPortPath;
		sdi->status = SR_ST_INACTIVE;
		sdi->conn = usb;
		sdi->inst_type = SR_INST_USB;

		devc = g_malloc0(sizeof(struct dev_context));
		sdi->priv = devc;

		{
			devc->model = &support_models[SLogic_BASIC_16_U3];

			devc->limit_samplechannel = devc->model->max_samplechannel;
			devc->limit_samplerate = devc->model->max_bandwidth / devc->model->max_samplechannel;

			devc->cur_samplechannel = devc->limit_samplechannel;
			devc->cur_samplerate = devc->limit_samplerate;

			devc->digital_group = sr_channel_group_new(sdi, "LA", NULL);
			for (i = 0; i < 16; i++) {
				channel_name = g_strdup_printf("D%u", i);
				ch = sr_channel_new(sdi, i, SR_CHANNEL_LOGIC, TRUE, channel_name);
				g_free(channel_name);
				devc->digital_group->channels = g_slist_append(
					devc->digital_group->channels, ch);
			}
		}

		devices = g_slist_append(devices, sdi);
	}
	// g_slist_free_full(conn_devices, (GDestroyNotify)sr_usb_dev_inst_free);

	return std_scan_complete(di, devices);
}

static int dev_open(struct sr_dev_inst *sdi)
{
	int ret;
	struct sr_usb_dev_inst *usb;
	struct dev_context *devc;
	struct sr_dev_driver *di;
	struct drv_context *drvc;


	if (!sdi) return SR_ERR_DEV_CLOSED;
	/* TODO: get handle from sdi->conn and open it. */
	usb  = sdi->conn;
	devc = sdi->priv;
	di	 = sdi->driver;
	drvc = di->context;

	ret = sr_usb_open(drvc->sr_ctx->libusb_ctx, usb);
	if (SR_OK != ret) return ret;

	ret = libusb_claim_interface(usb->devhdl, 0);
	if (ret != LIBUSB_SUCCESS) {
		switch (ret) {
		case LIBUSB_ERROR_BUSY:
			sr_err("Unable to claim USB interface. Another "
			       "program or driver has already claimed it.");
			break;
		case LIBUSB_ERROR_NO_DEVICE:
			sr_err("Device has been disconnected.");
			break;
		default:
			sr_err("Unable to claim interface: %s.",
			       libusb_error_name(ret));
			break;
		}
		return SR_ERR;
	}

	return std_dummy_dev_open(sdi);
}

static int dev_close(struct sr_dev_inst *sdi)
{
	int ret;
	struct sr_usb_dev_inst *usb;
	struct dev_context *devc;
	struct sr_dev_driver *di;
	struct drv_context *drvc;

	/* TODO: get handle from sdi->conn and close it. */
	usb  = sdi->conn;
	devc = sdi->priv;
	di	 = sdi->driver;
	drvc = di->context;

	ret = libusb_release_interface(usb->devhdl, 0);
	if (ret != LIBUSB_SUCCESS) {
		switch (ret) {
		case LIBUSB_ERROR_NO_DEVICE:
			sr_err("Device has been disconnected.");
			// return SR_ERR_DEV_CLOSED;
			break;
		default:
			sr_err("Unable to release Interface for %s.",
					libusb_error_name(ret));
			break;
		}
	}

	sr_usb_close(usb);

	return std_dummy_dev_close(sdi);
}

static int config_get(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	int ret;
	struct dev_context *devc;

	(void)cg;

	devc = sdi->priv;

	ret = SR_OK;
	switch (key) {
	case SR_CONF_SAMPLERATE:
		*data = g_variant_new_uint64(devc->cur_samplerate);
		break;
	case SR_CONF_BUFFERSIZE:
		*data = g_variant_new_uint64(devc->cur_samplechannel);
		break;
	case SR_CONF_LIMIT_SAMPLES:
		*data = g_variant_new_uint64(devc->cur_limit_samples);
		break;
	default:
		return SR_ERR_NA;
	}

	return ret;
}

static int config_set(uint32_t key, GVariant *data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	int ret;
	struct dev_context *devc;

	(void)cg;

	devc = sdi->priv;

	ret = SR_OK;
	switch (key) {
	case SR_CONF_SAMPLERATE:
		if (g_variant_get_uint64(data) > devc->limit_samplerate || std_u64_idx(data, ARRAY_AND_SIZE(samplerates)) < 0) {
			devc->cur_samplerate = devc->limit_samplerate;
			sr_warn("Reach limit or not supported, wrap to %uMHz.", devc->limit_samplerate/SR_MHZ(1));
		} else {
			devc->cur_samplerate = g_variant_get_uint64(data);
		}
		devc->limit_samplechannel = devc->model->max_bandwidth / devc->cur_samplerate;
		if (devc->limit_samplechannel > devc->model->max_samplechannel)
			devc->limit_samplechannel = devc->model->max_samplechannel;
		break;
	case SR_CONF_BUFFERSIZE:
		if (g_variant_get_uint64(data) > devc->limit_samplechannel || std_u64_idx(data, ARRAY_AND_SIZE(buffersizes)) < 0) {
			devc->cur_samplechannel = devc->limit_samplechannel;
			sr_warn("Reach limit or not supported, wrap to %uch.", devc->limit_samplechannel);
		} else {
			devc->cur_samplechannel = g_variant_get_uint64(data);
		}
		devc->limit_samplerate = devc->model->max_bandwidth / devc->cur_samplechannel;
		if (devc->limit_samplerate > devc->model->max_samplerate)
			devc->limit_samplerate = devc->model->max_samplerate;

		// [en|dis]able channels and dbg
		{
			for (GSList *l = devc->digital_group->channels; l; l = l->next) {
				struct sr_channel *ch = l->data;
				if (ch->type == SR_CHANNEL_LOGIC) { /* Might as well do this now, these are static. */
					sr_dev_channel_enable(ch, (ch->index >= devc->cur_samplechannel) ? FALSE : TRUE);
				} else {
					sr_warn("devc->digital_group->channels[%u] is not Logic?", ch->index);
				}
				sr_dbg("\tch[%2u] %-3s:%d %sabled priv:%p.", ch->index, ch->name, ch->type, ch->enabled?"en":"dis", ch->priv);
			}
		}
		break;
	case SR_CONF_LIMIT_SAMPLES:
		devc->cur_limit_samples = g_variant_get_uint64(data);
		break;
	default:
		ret = SR_ERR_NA;
	}

	return ret;
}

static int config_list(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	int ret;
	struct dev_context *devc;

	(void)cg;

	devc = sdi ? (sdi->priv) : NULL;

	ret = SR_OK;
	switch (key) {
	/* TODO */
	case SR_CONF_SCAN_OPTIONS:
	case SR_CONF_DEVICE_OPTIONS:
		ret = STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
		break;
	case SR_CONF_SAMPLERATE:
		*data = std_gvar_samplerates(samplerates, 1+std_u64_idx(g_variant_new_uint64(devc->limit_samplerate), ARRAY_AND_SIZE(samplerates)));
		break;
	case SR_CONF_BUFFERSIZE:
		*data = std_gvar_array_u64(buffersizes, 1+std_u64_idx(g_variant_new_uint64(devc->limit_samplechannel), ARRAY_AND_SIZE(buffersizes)));
		break;
	case SR_CONF_TRIGGER_MATCH:
		*data = std_gvar_array_i32(ARRAY_AND_SIZE(trigger_matches));
		break;
	default:
		ret = SR_ERR_NA;
	}

	return ret;
}

static struct sr_dev_driver sipeed_slogic_analyzer_driver_info = {
	.name = "sipeed-slogic-analyzer",
	.longname = "Sipeed Slogic Analyzer",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = std_dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = dev_open,
	.dev_close = dev_close,
	.dev_acquisition_start = sipeed_slogic_acquisition_start,
	.dev_acquisition_stop = sipeed_slogic_acquisition_stop,
	.context = NULL,
};
SR_REGISTER_DEV_DRIVER(sipeed_slogic_analyzer_driver_info);
