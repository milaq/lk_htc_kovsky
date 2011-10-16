/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <debug.h>
#include <string.h>
#include <stdlib.h>
#include <kernel/thread.h>
#include <kernel/event.h>
#include <dev/udc.h>

void boot_linux(void *bootimg, unsigned sz);

/* todo: give lk strtoul and nuke this */
static unsigned hex2unsigned(const char *x)
{
	unsigned n = 0;

	while (*x) {
		switch (*x) {
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			n = (n << 4) | (*x - '0');
			break;
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f':
			n = (n << 4) | (*x - 'a' + 10);
			break;
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
			n = (n << 4) | (*x - 'A' + 10);
			break;
		default:
			return n;
		}
		x++;
	}

	return n;
}

struct fastboot_cmd {
	struct fastboot_cmd *next;
	const char *prefix;
	unsigned prefix_len;
	void (*handle) (const char *arg, void *data, unsigned sz);
};

struct fastboot_var {
	struct fastboot_var *next;
	const char *name;
	const char *value;
};

static struct fastboot_cmd *cmdlist;

void
fastboot_register(const char *prefix,
		  void (*handle) (const char *arg, void *data, unsigned sz))
{
	struct fastboot_cmd *cmd;
	cmd = malloc(sizeof(*cmd));
	if (cmd) {
		cmd->prefix = prefix;
		cmd->prefix_len = strlen(prefix);
		cmd->handle = handle;
		cmd->next = cmdlist;
		cmdlist = cmd;
	}
}

static struct fastboot_var *varlist;

void fastboot_publish(const char *name, const char *value)
{
	struct fastboot_var *var;
	var = malloc(sizeof(*var));
	if (var) {
		var->name = name;
		var->value = value;
		var->next = varlist;
		varlist = var;
	}
}

#define USB_BUF_SZ 4096

static event_t usb_online;
static event_t tx_done;
static event_t rx_done;
static unsigned char buffer[USB_BUF_SZ];
static struct udc_endpoint *in, *out;
static struct udc_request *req_rx;
static struct udc_request *req_tx;
static int tx_status, rx_status;

static void *download_base;
static unsigned download_max;
static unsigned download_size;

#define STATE_OFFLINE	0
#define STATE_COMMAND	1
#define STATE_COMPLETE	2
#define STATE_ERROR	3

static unsigned fastboot_state = STATE_OFFLINE;

static void rx_complete(struct udc_request *_req, unsigned actual, int status)
{
	rx_status = status;
	_req->length = actual;
	event_signal(&rx_done, 0);
}

static void tx_complete(struct udc_request *_req, unsigned actual, int status)
{
	tx_status = status;
	_req->length = actual;
	event_signal(&tx_done, 0);
}

static int usb_read(void *_buf, unsigned len)
{
	int r;
	unsigned xfer;
	unsigned char *buf = _buf;
	int count = 0;

	if (fastboot_state == STATE_ERROR
		|| fastboot_state == STATE_OFFLINE)
		goto oops;

	while (len > 0) {
		xfer = (len > USB_BUF_SZ) ? USB_BUF_SZ : len;
		req_rx->buf = buf;
		req_rx->length = xfer;
		req_rx->complete = rx_complete;
		r = udc_request_queue(out, req_rx);
		if (r < 0) {
			dprintf(INFO, "usb_read() queue failed\n");
			goto oops;
		}
		event_wait(&rx_done);

		if (rx_status < 0) {
			dprintf(INFO, "usb_read() transaction failed\n");
			goto oops;
		}

		count += req_rx->length;
		buf += req_rx->length;
		len -= req_rx->length;

		/* short transfer? */
		if (req_rx->length != xfer) {
			dprintf(INFO, "%s: short read\n", __func__);
			break;
		}
	}

	return count;

oops:
	printf("[fastboot] %s: oops\n", __func__);
	fastboot_state = STATE_ERROR;
	return -1;
}

static int usb_write(void *buf, unsigned len)
{
	int r;

	if (fastboot_state == STATE_ERROR
		|| fastboot_state == STATE_OFFLINE)
		goto oops;
	
	req_tx->buf = buf;
	req_tx->length = len;
	req_tx->complete = tx_complete;
	r = udc_request_queue(in, req_tx);
	if (r < 0) {
		dprintf(INFO, "usb_write() queue failed\n");
		goto oops;
	}
	event_wait(&tx_done);
	if (tx_status < 0) {
		dprintf(INFO, "usb_write() transaction failed\n");
		goto oops;
	}
	return req_tx->length;

 oops:
	printf("[fastboot] %s: oops\n", __func__);
	fastboot_state = STATE_ERROR;
	return -1;
}

void fastboot_ack(const char *code, const char *reason)
{
	char response[64];

//	if (fastboot_state != STATE_COMMAND)
//		return;

	if (reason == 0)
		reason = "";

	snprintf(response, 64, "%s%s", code, reason);
	fastboot_state = STATE_COMPLETE;

	usb_write(response, strlen(response));

}

void fastboot_fail(const char *reason)
{
	fastboot_ack("FAIL", reason);
}

void fastboot_okay(const char *info)
{
	fastboot_ack("OKAY", info);
}

static void cmd_getvar(const char *arg, void *data, unsigned sz)
{
	struct fastboot_var *var;

	for (var = varlist; var; var = var->next) {
		if (!strcmp(var->name, arg)) {
			fastboot_okay(var->value);
			return;
		}
	}
	fastboot_okay("");
}

static void cmd_download(const char *arg, void *data, unsigned sz)
{
	char response[64];
	unsigned len = hex2unsigned(arg);
	int r;

	download_size = 0;
	if (len > download_max) {
		fastboot_fail("data too large");
		return;
	}

	sprintf(response, "DATA%08x", len);
	if (usb_write(response, strlen(response)) < 0)
		return;

	r = usb_read(download_base, len);
	if ((r < 0) || ((unsigned)r != len)) {
		fastboot_state = STATE_ERROR;
		return;
	}
	download_size = len;
	fastboot_okay("");
}

static void fastboot_command_loop(void)
{
	struct fastboot_cmd *cmd;
	int r;
	dprintf(INFO, "fastboot: processing commands\n");

 again:
	while (fastboot_state != STATE_ERROR
			&& fastboot_state != STATE_OFFLINE) {
		r = usb_read(buffer, 64);
		if (r < 0)
			break;
		buffer[r] = 0;
		dprintf(INFO, "fastboot: %s\n", buffer);

		for (cmd = cmdlist; cmd; cmd = cmd->next) {
			if (memcmp(buffer, cmd->prefix, cmd->prefix_len))
				continue;
			fastboot_state = STATE_COMMAND;
			cmd->handle((const char *)buffer + cmd->prefix_len,
				    (void *)download_base, download_size);
			if (fastboot_state == STATE_COMMAND)
				fastboot_fail("unknown reason");
			goto again;
		}

		fastboot_fail("unknown command");

	}
	if (fastboot_state == STATE_ERROR)
		dprintf(INFO, "fastboot: error\n");
}

static int fastboot_handler(void *arg)
{
	for (;;) {
		event_wait(&usb_online);
		fastboot_command_loop();
	}
	return 0;
}

static void fastboot_notify(struct udc_gadget *gadget, unsigned event)
{
	if (event == UDC_EVENT_ONLINE) {
		fastboot_state = STATE_COMPLETE;
		event_signal(&usb_online, 0);
	}
	else if (event == UDC_EVENT_OFFLINE) {
		fastboot_state = STATE_OFFLINE;
	}
}

static struct udc_endpoint *fastboot_endpoints[2];

static struct udc_gadget fastboot_gadget = {
	.notify = fastboot_notify,
	.ifc_class = 0xff,
	.ifc_subclass = 0x42,
	.ifc_protocol = 0x03,
	.ifc_endpoints = 2,
	.ifc_string = "fastboot",
	.ept = fastboot_endpoints,
};

int fastboot_init(void *base, unsigned size)
{
	thread_t *thr;
	dprintf(INFO, "fastboot_init()\n");

	download_base = base;
	download_max = size;

	event_init(&usb_online, 0, EVENT_FLAG_AUTOUNSIGNAL);
	event_init(&tx_done, 0, EVENT_FLAG_AUTOUNSIGNAL);
	event_init(&rx_done, 0, EVENT_FLAG_AUTOUNSIGNAL);

	in = udc_endpoint_alloc(UDC_TYPE_BULK_IN, 512);
	if (!in)
		goto fail_alloc_in;
	out = udc_endpoint_alloc(UDC_TYPE_BULK_OUT, 512);
	if (!out)
		goto fail_alloc_out;

	fastboot_endpoints[0] = in;
	fastboot_endpoints[1] = out;

	req_rx = udc_request_alloc();
	if (!req_rx)
		goto fail_alloc_rx;
	
	req_tx = udc_request_alloc();
	if (!req_tx)
		goto fail_alloc_tx;

	if (udc_register_gadget(&fastboot_gadget))
		goto fail_udc_register;

	fastboot_register("getvar:", cmd_getvar);
	fastboot_register("download:", cmd_download);
	fastboot_publish("version", "0.5");

	thr =
	    thread_create("fastboot", fastboot_handler, 0, DEFAULT_PRIORITY,
			  4096);
	thread_resume(thr);
	return 0;

 fail_udc_register:
 	udc_request_free(req_tx);
 fail_alloc_tx:
	udc_request_free(req_rx);
 fail_alloc_rx:
	udc_endpoint_free(out);
 fail_alloc_out:
	udc_endpoint_free(in);
 fail_alloc_in:
	return -1;
}
