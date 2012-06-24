/* spice-server spicevmc passthrough channel code

   Copyright (C) 2011 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <netinet/in.h> // IPPROTO_TCP
#include <netinet/tcp.h> // TCP_NODELAY

#include "char_device.h"
#include "red_channel.h"
#include "reds.h"

/* 64K should be enough for all but the largest writes + 32 bytes hdr */
#define BUF_SIZE (64 * 1024 + 32)

typedef struct SpiceVmcPipeItem {
    PipeItem base;
    uint32_t refs;

    /* writes which don't fit this will get split, this is not a problem */
    uint8_t buf[BUF_SIZE];
    uint32_t buf_used;
} SpiceVmcPipeItem;

typedef struct SpiceVmcState {
    RedChannel channel; /* Must be the first item */
    RedChannelClient *rcc;
    SpiceCharDeviceState *chardev_st;
    SpiceCharDeviceInstance *chardev_sin;
    SpiceVmcPipeItem *pipe_item;
    uint8_t *rcv_buf;
    uint32_t rcv_buf_size;
    int rcv_buf_in_use;
} SpiceVmcState;

static SpiceVmcPipeItem *spicevmc_pipe_item_ref(SpiceVmcPipeItem *item)
{
    item->refs++;
    return item;
}

static void spicevmc_pipe_item_unref(SpiceVmcPipeItem *item)
{
    if (!--item->refs) {
        free(item);
    }
}

SpiceCharDeviceMsgToClient *spicevmc_chardev_ref_msg_to_client(SpiceCharDeviceMsgToClient *msg,
                                                               void *opaque)
{
    return spicevmc_pipe_item_ref((SpiceVmcPipeItem *)msg);
}

static void spicevmc_chardev_unref_msg_to_client(SpiceCharDeviceMsgToClient *msg,
                                                 void *opaque)
{
    spicevmc_pipe_item_unref((SpiceVmcPipeItem *)msg);
}

static SpiceCharDeviceMsgToClient *spicevmc_chardev_read_msg_from_dev(SpiceCharDeviceInstance *sin,
                                                                      void *opaque)
{
    SpiceVmcState *state = opaque;
    SpiceCharDeviceInterface *sif;
    SpiceVmcPipeItem *msg_item;
    int n;

    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    if (!state->rcc) {
        return NULL;
    }

    if (!state->pipe_item) {
        msg_item = spice_new0(SpiceVmcPipeItem, 1);
        msg_item->refs = 1;
        red_channel_pipe_item_init(&state->channel,
                                       &msg_item->base, 0);
    } else {
        spice_assert(state->pipe_item->buf_used == 0);
        msg_item = state->pipe_item;
        state->pipe_item = NULL;
    }

    n = sif->read(sin, msg_item->buf,
                  sizeof(msg_item->buf));
    if (n > 0) {
        spice_debug("read from dev %d", n);
        msg_item->buf_used = n;
        return msg_item;
    } else {
        state->pipe_item = msg_item;
        return NULL;
    }
}

static void spicevmc_chardev_send_msg_to_client(SpiceCharDeviceMsgToClient *msg,
                                                 RedClient *client,
                                                 void *opaque)
{
    SpiceVmcState *state = opaque;
    SpiceVmcPipeItem *vmc_msg = msg;

    spice_assert(state->rcc->client == client);
    spicevmc_pipe_item_ref(vmc_msg);
    red_channel_client_pipe_add_push(state->rcc, &vmc_msg->base);
}

static void spicevmc_char_dev_send_tokens_to_client(RedClient *client,
                                                    uint32_t tokens,
                                                    void *opaque)
{
    spice_printerr("Not implemented!");
}

static void spicevmc_char_dev_remove_client(RedClient *client, void *opaque)
{
    SpiceVmcState *state = opaque;

    spice_printerr("vmc state %p, client %p", state, client);
    spice_assert(state->rcc && state->rcc->client == client);

    red_channel_client_shutdown(state->rcc);
}

static int spicevmc_red_channel_client_config_socket(RedChannelClient *rcc)
{
    int delay_val = 1;
    RedsStream *stream = red_channel_client_get_stream(rcc);

    if (rcc->channel->type == SPICE_CHANNEL_USBREDIR) {
        if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY,
                &delay_val, sizeof(delay_val)) != 0) {
            if (errno != ENOTSUP && errno != ENOPROTOOPT) {
                spice_printerr("setsockopt failed, %s", strerror(errno));
                return FALSE;
            }
        }
    }

    return TRUE;
}

static void spicevmc_red_channel_client_on_disconnect(RedChannelClient *rcc)
{
    SpiceVmcState *state;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    if (!rcc) {
        return;
    }

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);
    sin = state->chardev_sin;
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    if (state->chardev_st) {
        if (spice_char_device_client_exists(state->chardev_st, rcc->client)) {
            spice_char_device_client_remove(state->chardev_st, rcc->client);
        } else {
            spice_printerr("client %p have already been removed from char dev %p",
                           rcc->client, state->chardev_st);
        }
    }

    /* Don't destroy the rcc if it is already being destroyed, as then
       red_client_destroy/red_channel_client_destroy will already do this! */
    if (!rcc->destroying)
        red_channel_client_destroy(rcc);

    state->rcc = NULL;
    if (sif->state) {
        sif->state(sin, 0);
    }
}

static int spicevmc_red_channel_client_handle_message(RedChannelClient *rcc,
                                                      uint16_t type,
                                                      uint32_t size,
                                                      uint8_t *msg)
{
    SpiceVmcState *state;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);
    sin = state->chardev_sin;
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    if (type != SPICE_MSGC_SPICEVMC_DATA) {
        return red_channel_client_handle_message(rcc, size, type, msg);
    }

    /*
     * qemu spicevmc will consume everything we give it, no need for
     * flow control checks (or to use a pipe).
     */
    sif->write(sin, msg, size);

    return TRUE;
}

static uint8_t *spicevmc_red_channel_alloc_msg_rcv_buf(RedChannelClient *rcc,
                                                       uint16_t type,
                                                       uint32_t size)
{
    SpiceVmcState *state;

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);

    assert(!state->rcv_buf_in_use);

    if (size > state->rcv_buf_size) {
        state->rcv_buf = spice_realloc(state->rcv_buf, size);
        state->rcv_buf_size = size;
    }

    state->rcv_buf_in_use = 1;

    return state->rcv_buf;
}

static void spicevmc_red_channel_release_msg_rcv_buf(RedChannelClient *rcc,
                                                     uint16_t type,
                                                     uint32_t size,
                                                     uint8_t *msg)
{
    SpiceVmcState *state;

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);

    /* NOOP, we re-use the buffer every time and only free it on destruction */
    state->rcv_buf_in_use = 0;
}

static void spicevmc_red_channel_hold_pipe_item(RedChannelClient *rcc,
    PipeItem *item)
{
    /* NOOP */
}

static void spicevmc_red_channel_send_item(RedChannelClient *rcc,
    PipeItem *item)
{
    SpiceVmcPipeItem *i = SPICE_CONTAINEROF(item, SpiceVmcPipeItem, base);
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);

    red_channel_client_init_send_data(rcc, SPICE_MSG_SPICEVMC_DATA, item);
    spice_marshaller_add_ref(m, i->buf, i->buf_used);
    red_channel_client_begin_send_message(rcc);
}

static void spicevmc_red_channel_release_pipe_item(RedChannelClient *rcc,
    PipeItem *item, int item_pushed)
{
    spicevmc_pipe_item_unref((SpiceVmcPipeItem *)item);
}

static void spicevmc_connect(RedChannel *channel, RedClient *client,
    RedsStream *stream, int migration, int num_common_caps,
    uint32_t *common_caps, int num_caps, uint32_t *caps)
{
    RedChannelClient *rcc;
    SpiceVmcState *state;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    state = SPICE_CONTAINEROF(channel, SpiceVmcState, channel);
    sin = state->chardev_sin;
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceCharDeviceInterface, base);

    if (state->rcc) {
        spice_printerr("channel client %d:%d (%p) already connected, refusing second connection",
                       channel->type, channel->id, state->rcc);
        // TODO: notify client in advance about the in use channel using
        // SPICE_MSG_MAIN_CHANNEL_IN_USE (for example)
        reds_stream_free(stream);
        return;
    }

    rcc = red_channel_client_create(sizeof(RedChannelClient), channel, client, stream,
                                    num_common_caps, common_caps,
                                    num_caps, caps);
    if (!rcc) {
        return;
    }
    state->rcc = rcc;
    red_channel_client_ack_zero_messages_window(rcc);

    spice_char_device_client_add(state->chardev_st, client, FALSE, 0, ~0, ~0);

    if (sif->state) {
        sif->state(sin, 1);
    }
}

static void spicevmc_migrate(RedChannelClient *rcc)
{
    /* NOOP */
}

SpiceCharDeviceState *spicevmc_device_connect(SpiceCharDeviceInstance *sin,
                                              uint8_t channel_type)
{
    static uint8_t id[256] = { 0, };
    SpiceVmcState *state;
    ChannelCbs channel_cbs = { NULL, };
    ClientCbs client_cbs = { NULL, };
    SpiceCharDeviceCallbacks char_dev_cbs = {NULL, };

    channel_cbs.config_socket = spicevmc_red_channel_client_config_socket;
    channel_cbs.on_disconnect = spicevmc_red_channel_client_on_disconnect;
    channel_cbs.send_item = spicevmc_red_channel_send_item;
    channel_cbs.hold_item = spicevmc_red_channel_hold_pipe_item;
    channel_cbs.release_item = spicevmc_red_channel_release_pipe_item;
    channel_cbs.alloc_recv_buf = spicevmc_red_channel_alloc_msg_rcv_buf;
    channel_cbs.release_recv_buf = spicevmc_red_channel_release_msg_rcv_buf;

    state = (SpiceVmcState*)red_channel_create(sizeof(SpiceVmcState),
                                   core, channel_type, id[channel_type]++,
                                   FALSE /* migration - TODO? */,
                                   FALSE /* handle_acks */,
                                   spicevmc_red_channel_client_handle_message,
                                   &channel_cbs);
    red_channel_init_outgoing_messages_window(&state->channel);

    client_cbs.connect = spicevmc_connect;
    client_cbs.migrate = spicevmc_migrate;
    red_channel_register_client_cbs(&state->channel, &client_cbs);

    char_dev_cbs.read_one_msg_from_device = spicevmc_chardev_read_msg_from_dev;
    char_dev_cbs.ref_msg_to_client = spicevmc_chardev_ref_msg_to_client;
    char_dev_cbs.unref_msg_to_client = spicevmc_chardev_unref_msg_to_client;
    char_dev_cbs.send_msg_to_client = spicevmc_chardev_send_msg_to_client;
    char_dev_cbs.send_tokens_to_client = spicevmc_char_dev_send_tokens_to_client;
    char_dev_cbs.remove_client = spicevmc_char_dev_remove_client;

    state->chardev_st = spice_char_device_state_create(sin,
                                                       0, /* tokens interval */
                                                       ~0, /* self tokens */
                                                       &char_dev_cbs,
                                                       state);
    state->chardev_sin = sin;

    reds_register_channel(&state->channel);
    return state->chardev_st;
}

/* Must be called from RedClient handling thread. */
void spicevmc_device_disconnect(SpiceCharDeviceInstance *sin)
{
    SpiceVmcState *state;

    state = (SpiceVmcState *)spice_char_device_state_opaque_get(sin->st);

    spice_char_device_state_destroy(sin->st);
    state->chardev_st = NULL;

    reds_unregister_channel(&state->channel);

    free(state->pipe_item);
    free(state->rcv_buf);
    red_channel_destroy(&state->channel);
}
