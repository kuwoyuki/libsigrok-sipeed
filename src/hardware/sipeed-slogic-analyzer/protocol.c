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

static void LIBUSB_CALL receive_transfer(struct libusb_transfer *transfer) {

	int ret;
	const struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct sr_usb_dev_inst *usb;

	sdi  = transfer->user_data;
	if (!sdi)
		return;
	devc = sdi->priv;
	usb  = sdi->conn;

	int64_t transfers_reached_time_now = g_get_monotonic_time();
	int64_t transfers_reached_duration = transfers_reached_time_now - devc->transfers_reached_time_latest;

	if (devc->acq_aborted == 1)
		return;

	sr_spew("Transfer[%d] status: %d(%s)", std_u64_idx(g_variant_new_uint64((uint64_t)transfer), (uint64_t*)devc->transfers, NUM_MAX_TRANSFERS),
		transfer->status, libusb_error_name(transfer->status));
	switch (transfer->status) {
		case LIBUSB_TRANSFER_COMPLETED: 
		case LIBUSB_TRANSFER_TIMED_OUT: { /* may have received some data */
			devc->transfers_reached_nbytes_latest = transfer->actual_length;
			devc->transfers_reached_nbytes += devc->transfers_reached_nbytes_latest;
			if (transfer->actual_length > devc->samples_need_nbytes - devc->samples_got_nbytes)
				transfer->actual_length = devc->samples_need_nbytes - devc->samples_got_nbytes;
			devc->samples_got_nbytes += transfer->actual_length;
			sr_dbg("[%u] Got(%.2f%%): %u/%u => speed: %.2fMBps, %.2fMBps(avg) => +%.3f=%.3fms.",
				devc->num_transfers_completed,
				100.f * devc->samples_got_nbytes / devc->samples_need_nbytes, devc->samples_got_nbytes, devc->samples_need_nbytes,
				(double)devc->transfers_reached_nbytes_latest / transfers_reached_duration,
				(double)devc->transfers_reached_nbytes / (transfers_reached_time_now - devc->transfers_reached_time_start),
				(double)transfers_reached_duration / SR_KHZ(1),
				(double)(transfers_reached_time_now - devc->transfers_reached_time_start) / SR_KHZ(1)
			);
			devc->transfers_reached_time_latest = transfers_reached_time_now;

			if (transfer->actual_length == 0) {
				devc->num_transfers_used -= 1;
				break;
			}

			/* TODO: move out submit to ensure continuous transfers */
			if (devc->cur_pattern_mode_idx != PATTERN_MODE_TEST_MAX_SPEED) {
				uint8_t * d = transfer->buffer;
				size_t len = transfer->actual_length;
				// sr_dbg("HEAD: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
				// 	d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
				devc->model->submit_raw_data(d, len, sdi);
			}

			devc->num_transfers_used -= 1;
			if (devc->samples_got_nbytes + devc->num_transfers_used * devc->per_transfer_nbytes < devc->samples_need_nbytes) {
				transfer->actual_length = 0;
				transfer->timeout = (TRANSFERS_DURATION_TOLERANCE + 1) * devc->per_transfer_duration * (devc->num_transfers_used + 2);
				ret = libusb_submit_transfer(transfer);
				if (ret) {
					sr_dbg("Failed to submit transfer: %s", libusb_error_name(ret));
				} else {
					sr_spew("Resubmit transfer: %p", transfer);
					devc->num_transfers_used += 1;
				}
			}
		} break;

		case LIBUSB_TRANSFER_OVERFLOW:
		case LIBUSB_TRANSFER_STALL:
		case LIBUSB_TRANSFER_NO_DEVICE: {
			devc->num_transfers_used = 0;
		} break;

		default: {
			devc->num_transfers_used -= 1;
		} break;
	}

	if (devc->num_transfers_completed && (double)transfers_reached_duration / SR_KHZ(1) > (TRANSFERS_DURATION_TOLERANCE + 1) * devc->per_transfer_duration) {
		sr_err("Timeout %.3fms!!! Reach duration limit: %.3f(%u+%.1f%%) except first one.",
			(double)transfers_reached_duration / SR_KHZ(1),
			(TRANSFERS_DURATION_TOLERANCE + 1) * devc->per_transfer_duration, devc->per_transfer_duration, TRANSFERS_DURATION_TOLERANCE * 100
		);
		devc->num_transfers_used = 0;
	}

	if (devc->num_transfers_used == 0) {
		sipeed_slogic_acquisition_stop(sdi);
	}

	devc->num_transfers_completed += 1;
};

static int handle_events(int fd, int revents, void *cb_data)
{
	struct sr_dev_inst *sdi;
	struct sr_dev_driver *di;
	struct dev_context *devc;
	struct drv_context *drvc;

	(void)fd;
	(void)revents;

	sdi = cb_data;
	devc = sdi->priv;
	di = sdi->driver;
	drvc = di->context;

	//sr_spew("handle_events enter");

	if (devc->acq_aborted) {
		for (size_t i = 0; i < NUM_MAX_TRANSFERS; ++i) {
			struct libusb_transfer *transfer = devc->transfers[i];
			if (transfer) {
				libusb_cancel_transfer(transfer);
			}
		}
		for (size_t i = 0; i < NUM_MAX_TRANSFERS; ++i) {
			libusb_handle_events_timeout_completed(drvc->sr_ctx->libusb_ctx, &(struct timeval){0, 0}, NULL);
			struct libusb_transfer *transfer = devc->transfers[i];
			if (transfer) {
				g_free(transfer->buffer);
				libusb_free_transfer(transfer);
			}
			devc->transfers[i] = NULL;
		}

		sr_dbg("Freed all transfers.");
		sr_info("Bulk in %u/%u bytes with %u transfers.", devc->samples_got_nbytes, devc->samples_need_nbytes, devc->num_transfers_completed);

		sr_session_source_remove(sdi->session, -1 * (size_t)drvc->sr_ctx->libusb_ctx);
		std_session_send_df_end(sdi);
	}

	libusb_handle_events_timeout_completed(drvc->sr_ctx->libusb_ctx, &(struct timeval){0, 0}, NULL);

	return TRUE;
}

SR_PRIV int sipeed_slogic_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct sr_dev_driver *di;
	struct dev_context *devc;
	struct drv_context *drvc;
	struct sr_usb_dev_inst *usb;

	int ret;

	devc = sdi->priv;
	di = sdi->driver;
	drvc = di->context;
	usb  = sdi->conn;

	if ((ret = devc->model->operation.remote_stop(sdi)) < 0) {
		sr_err("Unhandled `CMD_STOP`");
		return ret;
	}
	// clear_ep(sdi);

	devc->samples_got_nbytes = 0;
	devc->samples_need_nbytes = devc->cur_limit_samples * devc->cur_samplechannel / 8;
	sr_info("Need %ux %uch@%uMHz in %ums.", 
			devc->cur_limit_samples,
			devc->cur_samplechannel, 
			devc->cur_samplerate / SR_MHZ(1),
			1000 * devc->cur_limit_samples / devc->cur_samplerate
	);

	devc->per_transfer_duration = 125;
	devc->per_transfer_nbytes = devc->per_transfer_duration * devc->cur_samplerate * devc->cur_samplechannel / 8 / SR_KHZ(1) /* ms */;

	do {
		struct libusb_transfer *transfer = libusb_alloc_transfer(0);
		if (!transfer) {
			sr_err("Failed to allocate libusb transfer!");
			return SR_ERR_IO;
		}
		do {
			devc->per_transfer_nbytes = (devc->per_transfer_nbytes + (2*16*1024-1)) & ~(2*16*1024-1);
			devc->per_transfer_duration = devc->per_transfer_nbytes * SR_KHZ(1) * 8 / (devc->cur_samplerate * devc->cur_samplechannel);
			sr_dbg("Plan to receive %u bytes per %ums...", devc->per_transfer_nbytes, devc->per_transfer_duration);
			
			uint8_t *dev_buf = g_malloc(devc->per_transfer_nbytes);
			if (!dev_buf) {
				sr_dbg("Failed to allocate memory: %u bytes! Half .", devc->per_transfer_nbytes);
				devc->per_transfer_nbytes >>= 1;
				continue;
			}

			libusb_fill_bulk_transfer(transfer, usb->devhdl, devc->model->ep_in,
										dev_buf, devc->per_transfer_nbytes, NULL,
										NULL, 0);

			ret = libusb_submit_transfer(transfer);
			if (ret) {
				g_free(transfer->buffer);
				if (ret == LIBUSB_ERROR_NO_MEM) {
					sr_dbg("Failed to submit transfer: %s!", libusb_error_name(ret));
					devc->per_transfer_nbytes >>= 1;
					continue;
				} else {
					sr_err("Failed to submit transfer: %s!", libusb_error_name(ret));
					libusb_free_transfer(transfer);
					return SR_ERR_IO;
				}
			} else {
				ret = libusb_cancel_transfer(transfer);
				if (ret) {
                    sr_dbg("Failed to cancel transfer: %s!", libusb_error_name(ret));
                }
                libusb_handle_events_timeout_completed(drvc->sr_ctx->libusb_ctx, &(struct timeval){3, 0}, NULL);
				g_free(transfer->buffer);

				devc->per_transfer_nbytes >>= 1;
				devc->per_transfer_duration = devc->per_transfer_nbytes / (devc->cur_samplerate / SR_KHZ(1) * devc->cur_samplechannel / 8);
				break;
			}
		} while (devc->per_transfer_nbytes > 32*1024); // 32kiB > 125ms * 1MHZ * 2ch
		libusb_free_transfer(transfer);
		sr_info("Nice plan! :) => %u bytes per %ums.", devc->per_transfer_nbytes, devc->per_transfer_duration);
	} while (0);


	devc->acq_aborted = 0;
	devc->num_transfers_used = 0;
	devc->num_transfers_completed = 0;
	memset(devc->transfers, 0, sizeof(devc->transfers));
	devc->transfers_reached_nbytes = 0;

	sr_session_source_add(sdi->session, -1 * (size_t)drvc->sr_ctx->libusb_ctx, 0, (devc->per_transfer_duration / 2)?:1, handle_events, (void *)sdi);

	while (devc->num_transfers_used < NUM_MAX_TRANSFERS && devc->samples_got_nbytes + devc->num_transfers_used * devc->per_transfer_nbytes < devc->samples_need_nbytes)
	{
		uint8_t *dev_buf = g_malloc(devc->per_transfer_nbytes);
		if (!dev_buf) {
			sr_dbg("Failed to allocate memory[%d]", devc->num_transfers_used);
			break;
		}

		struct libusb_transfer *transfer = libusb_alloc_transfer(0);
		if (!transfer) {
			sr_dbg("Failed to allocate transfer[%d]", devc->num_transfers_used);
			g_free(dev_buf);
			break;
		}

		libusb_fill_bulk_transfer(transfer, usb->devhdl, devc->model->ep_in,
									dev_buf, devc->per_transfer_nbytes, receive_transfer,
									sdi, (TRANSFERS_DURATION_TOLERANCE + 1) * devc->per_transfer_duration * (devc->num_transfers_used + 2));
		transfer->actual_length = 0;

		ret = libusb_submit_transfer(transfer);
		if (ret) {
			sr_dbg("Failed to submit transfer[%d]: %s.", devc->num_transfers_used, libusb_error_name(ret));
			g_free(transfer->buffer);
			libusb_free_transfer(transfer);
			break;
		}
		devc->transfers[devc->num_transfers_used] = transfer;
		devc->num_transfers_used += 1;
	}
	sr_dbg("Submited %u transfers", devc->num_transfers_used);

	std_session_send_df_header(sdi);
	std_session_send_df_frame_begin(sdi);

	devc->transfers_reached_time_start = g_get_monotonic_time();
	devc->transfers_reached_time_latest = devc->transfers_reached_time_start;

	if (!devc->num_transfers_used) {
		sipeed_slogic_acquisition_stop(sdi);
		return SR_OK;
	}

	if ((ret = devc->model->operation.remote_run(sdi)) < 0) {
		sr_err("Unhandled `CMD_RUN`");
		return ret;
	}

	return SR_OK;
}

SR_PRIV int sipeed_slogic_acquisition_stop(struct sr_dev_inst *sdi)
{

	struct dev_context *devc;

	devc = sdi->priv;

	devc->acq_aborted = 1;

	return SR_OK;
}
