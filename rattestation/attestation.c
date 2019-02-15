/*
 * This file is part of trust|me
 * Copyright(c) 2013 - 2019 Fraunhofer AISEC
 * Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 (GPL 2), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Fraunhofer AISEC <trustme@aisec.fraunhofer.de>
 */

#include "attestation.pb-c.h"

#include "common/macro.h"
#include "common/mem.h"
#include "common/protobuf.h"
#include "common/event.h"
#include "common/sock.h"
#include "common/fd.h"

#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>

#include "attestation.h"

#define TPM2D_SERVICE_PORT "9505"

struct attestation_resp_cb_data {
	void (*resp_verified_cb)(bool);
	const char *nonce;
};

static bool
attestation_verify_resp(Tpm2dToRemote *resp, const char *nonce)
{
	DEBUG("Got Response from TPM2D");
	protobuf_dump_message(STDOUT_FILENO, (ProtobufCMessage *) resp);
	INFO("Response for Nonce %s, Response size=%zu", nonce,
		protobuf_c_message_get_packed_size((ProtobufCMessage *) resp));
	//TODO implement verification process
	return true;
}

static void
attestation_response_recv_cb(int fd, unsigned events, event_io_t *io, void *data)
{
	bool verified = false;
	struct attestation_resp_cb_data *resp_cb_data = data;

	if (events & EVENT_IO_EXCEPT) {
		WARN("Exception on connected socket to control client; closing socket");
		goto cleanup;
	}

	IF_FALSE_RETURN(events & EVENT_IO_READ);

	Tpm2dToRemote *resp = (Tpm2dToRemote *)
		protobuf_recv_message(fd, &tpm2d_to_remote__descriptor);
	IF_NULL_GOTO_ERROR(resp, cleanup);

	verified = attestation_verify_resp(resp, resp_cb_data->nonce);

	protobuf_free_message((ProtobufCMessage *)resp);
	INFO("Handled response on connection %d", fd);

cleanup:
	event_remove_io(io);
	event_io_free(io);
	if (close(fd) < 0)
		WARN_ERRNO("Failed to close connected tpm2d socket");
	// call registerd handler with verification result
	if (resp_cb_data->resp_verified_cb)
		(resp_cb_data->resp_verified_cb)(verified);
	mem_free(resp_cb_data);
}

int
attestation_do_request(const char *host, void (*resp_verified_cb)(bool))
{
	char *nonce = "deadbeef"; //FIXME generate a real nonce here

	// build RemoteToTpm2d message
	RemoteToTpm2d msg = REMOTE_TO_TPM2D__INIT;

	msg.code = REMOTE_TO_TPM2D__CODE__ATTESTATION_REQ;
	msg.qualifyingdata = nonce;

	int sock = sock_inet_create_and_connect(SOCK_STREAM, host, TPM2D_SERVICE_PORT);
	IF_TRUE_RETVAL(sock < 0, -1);

	DEBUG("Sending attestation request to TPM2D on %s:%s", host, TPM2D_SERVICE_PORT);

	ssize_t msg_size = protobuf_send_message(sock, (ProtobufCMessage *) &msg);
	IF_TRUE_RETVAL(msg_size < 0, -1);
	INFO("Request with Nonce %s, Request size=%zd", nonce, msg_size);

	struct attestation_resp_cb_data *resp_cb_data =
			mem_new0(struct attestation_resp_cb_data, 1);
	resp_cb_data->resp_verified_cb = resp_verified_cb;
	resp_cb_data->nonce = nonce;

	DEBUG("Register Response handler on sockfd=%d", sock);
	fd_make_non_blocking(sock);
	event_io_t *event = event_io_new(sock, EVENT_IO_READ,
			attestation_response_recv_cb, resp_cb_data);
	event_add_io(event);

	return 0;
}