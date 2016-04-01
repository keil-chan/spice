/* spice-server char device flow control code

   Copyright (C) 2012 Red Hat, Inc.

   Red Hat Authors:
   Yonit Halperin <yhalperi@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http:www.gnu.org/licenses/>.
*/


#include <config.h>
#include <inttypes.h>
#include "char-device.h"
#include "red-channel.h"
#include "reds.h"

#define CHAR_DEVICE_WRITE_TO_TIMEOUT 100
#define RED_CHAR_DEVICE_WAIT_TOKENS_TIMEOUT 30000
#define MAX_POOL_SIZE (10 * 64 * 1024)

typedef struct RedCharDeviceClient RedCharDeviceClient;
struct RedCharDeviceClient {
    RingItem link;
    RedCharDevice *dev;
    RedClient *client;
    int do_flow_control;
    uint64_t num_client_tokens;
    uint64_t num_client_tokens_free; /* client messages that were consumed by the device */
    uint64_t num_send_tokens; /* send to client */
    SpiceTimer *wait_for_tokens_timer;
    int wait_for_tokens_started;
    Ring send_queue;
    uint32_t send_queue_size;
    uint32_t max_send_queue_size;
};

struct RedCharDevicePrivate {
    int running;
    int active; /* has read/write been performed since the device was started */
    int wait_for_migrate_data;
    uint32_t refs;

    Ring write_queue;
    Ring write_bufs_pool;
    uint64_t cur_pool_size;
    RedCharDeviceWriteBuffer *cur_write_buf;
    uint8_t *cur_write_buf_pos;
    SpiceTimer *write_to_dev_timer;
    uint64_t num_self_tokens;

    Ring clients; /* list of RedCharDeviceClient */
    uint32_t num_clients;

    uint64_t client_tokens_interval; /* frequency of returning tokens to the client */
    SpiceCharDeviceInstance *sin;

    int during_read_from_device;
    int during_write_to_device;

    RedCharDeviceCallbacks cbs;
    void *opaque;
    SpiceServer *reds;
};

/* typedef'ed as RedCharDevice */
struct SpiceCharDeviceState {
    struct RedCharDevicePrivate priv[1];
};

enum {
    WRITE_BUFFER_ORIGIN_NONE,
    WRITE_BUFFER_ORIGIN_CLIENT,
    WRITE_BUFFER_ORIGIN_SERVER,
    WRITE_BUFFER_ORIGIN_SERVER_NO_TOKEN,
};

/* Holding references for avoiding access violation if the char device was
 * destroyed during a callback */
static void red_char_device_ref(RedCharDevice *char_dev);
static void red_char_device_unref(RedCharDevice *char_dev);
static void red_char_device_write_buffer_unref(RedCharDeviceWriteBuffer *write_buf);

static void red_char_device_write_retry(void *opaque);

typedef struct RedCharDeviceMsgToClientItem {
    RingItem link;
    RedCharDeviceMsgToClient *msg;
} RedCharDeviceMsgToClientItem;

static RedCharDeviceMsgToClient *
red_char_device_read_one_msg_from_device(RedCharDevice *dev)
{
   return dev->priv->cbs.read_one_msg_from_device(dev->priv->sin, dev->priv->opaque);
}

static RedCharDeviceMsgToClient *
red_char_device_ref_msg_to_client(RedCharDevice *dev,
                                  RedCharDeviceMsgToClient *msg)
{
   return dev->priv->cbs.ref_msg_to_client(msg, dev->priv->opaque);
}

static void
red_char_device_unref_msg_to_client(RedCharDevice *dev,
                                    RedCharDeviceMsgToClient *msg)
{
   dev->priv->cbs.unref_msg_to_client(msg, dev->priv->opaque);
}

static void
red_char_device_send_msg_to_client(RedCharDevice *dev,
                                   RedCharDeviceMsgToClient *msg,
                                   RedClient *client)
{
   dev->priv->cbs.send_msg_to_client(msg, client, dev->priv->opaque);
}

static void
red_char_device_send_tokens_to_client(RedCharDevice *dev,
                                      RedClient *client,
                                      uint32_t tokens)
{
   dev->priv->cbs.send_tokens_to_client(client, tokens, dev->priv->opaque);
}

static void
red_char_device_on_free_self_token(RedCharDevice *dev)
{
   if (dev->priv->cbs.on_free_self_token != NULL) {
       dev->priv->cbs.on_free_self_token(dev->priv->opaque);
   }
}

static void
red_char_device_remove_client(RedCharDevice *dev, RedClient *client)
{
   dev->priv->cbs.remove_client(client, dev->priv->opaque);
}

static void red_char_device_write_buffer_free(RedCharDeviceWriteBuffer *buf)
{
    if (buf == NULL)
        return;

    free(buf->buf);
    free(buf);
}

static void write_buffers_queue_free(Ring *write_queue)
{
    while (!ring_is_empty(write_queue)) {
        RingItem *item = ring_get_tail(write_queue);
        RedCharDeviceWriteBuffer *buf;

        ring_remove(item);
        buf = SPICE_CONTAINEROF(item, RedCharDeviceWriteBuffer, link);
        red_char_device_write_buffer_free(buf);
    }
}

static void red_char_device_write_buffer_pool_add(RedCharDevice *dev,
                                                  RedCharDeviceWriteBuffer *buf)
{
    if (buf->refs == 1 &&
        dev->priv->cur_pool_size < MAX_POOL_SIZE) {
        buf->buf_used = 0;
        buf->origin = WRITE_BUFFER_ORIGIN_NONE;
        buf->client = NULL;
        dev->priv->cur_pool_size += buf->buf_size;
        ring_add(&dev->priv->write_bufs_pool, &buf->link);
        return;
    }

    /* Buffer still being used - just unref for the caller */
    red_char_device_write_buffer_unref(buf);
}

static void red_char_device_client_send_queue_free(RedCharDevice *dev,
                                                   RedCharDeviceClient *dev_client)
{
    spice_debug("send_queue_empty %d", ring_is_empty(&dev_client->send_queue));
    while (!ring_is_empty(&dev_client->send_queue)) {
        RingItem *item = ring_get_tail(&dev_client->send_queue);
        RedCharDeviceMsgToClientItem *msg_item = SPICE_CONTAINEROF(item,
                                                                   RedCharDeviceMsgToClientItem,
                                                                   link);

        ring_remove(item);
        red_char_device_unref_msg_to_client(dev, msg_item->msg);
        free(msg_item);
    }
    dev_client->num_send_tokens += dev_client->send_queue_size;
    dev_client->send_queue_size = 0;
}

static void red_char_device_client_free(RedCharDevice *dev,
                                        RedCharDeviceClient *dev_client)
{
    RingItem *item, *next;

    if (dev_client->wait_for_tokens_timer) {
        reds_core_timer_remove(dev->priv->reds, dev_client->wait_for_tokens_timer);
        dev_client->wait_for_tokens_timer = NULL;
    }

    red_char_device_client_send_queue_free(dev, dev_client);

    /* remove write buffers that are associated with the client */
    spice_debug("write_queue_is_empty %d", ring_is_empty(&dev->priv->write_queue) && !dev->priv->cur_write_buf);
    RING_FOREACH_SAFE(item, next, &dev->priv->write_queue) {
        RedCharDeviceWriteBuffer *write_buf;

        write_buf = SPICE_CONTAINEROF(item, RedCharDeviceWriteBuffer, link);
        if (write_buf->origin == WRITE_BUFFER_ORIGIN_CLIENT &&
            write_buf->client == dev_client->client) {
            ring_remove(item);
            red_char_device_write_buffer_pool_add(dev, write_buf);
        }
    }

    if (dev->priv->cur_write_buf && dev->priv->cur_write_buf->origin == WRITE_BUFFER_ORIGIN_CLIENT &&
        dev->priv->cur_write_buf->client == dev_client->client) {
        dev->priv->cur_write_buf->origin = WRITE_BUFFER_ORIGIN_NONE;
        dev->priv->cur_write_buf->client = NULL;
    }

    dev->priv->num_clients--;
    ring_remove(&dev_client->link);
    free(dev_client);
}

static void red_char_device_handle_client_overflow(RedCharDeviceClient *dev_client)
{
    RedCharDevice *dev = dev_client->dev;
    spice_printerr("dev %p client %p ", dev, dev_client);
    red_char_device_remove_client(dev, dev_client->client);
}

static RedCharDeviceClient *red_char_device_client_find(RedCharDevice *dev,
                                                        RedClient *client)
{
    RingItem *item;

    RING_FOREACH(item, &dev->priv->clients) {
        RedCharDeviceClient *dev_client;

        dev_client = SPICE_CONTAINEROF(item, RedCharDeviceClient, link);
        if (dev_client->client == client) {
            return dev_client;
        }
    }
    return NULL;
}

/***************************
 * Reading from the device *
 **************************/

static void device_client_wait_for_tokens_timeout(void *opaque)
{
    RedCharDeviceClient *dev_client = opaque;

    red_char_device_handle_client_overflow(dev_client);
}

static int red_char_device_can_send_to_client(RedCharDeviceClient *dev_client)
{
    return !dev_client->do_flow_control || dev_client->num_send_tokens;
}

static uint64_t red_char_device_max_send_tokens(RedCharDevice *dev)
{
    RingItem *item;
    uint64_t max = 0;

    RING_FOREACH(item, &dev->priv->clients) {
        RedCharDeviceClient *dev_client;

        dev_client = SPICE_CONTAINEROF(item, RedCharDeviceClient, link);

        if (!dev_client->do_flow_control) {
            max = ~0;
            break;
        }

        if (dev_client->num_send_tokens > max) {
            max = dev_client->num_send_tokens;
        }
    }
    return max;
}

static void red_char_device_add_msg_to_client_queue(RedCharDeviceClient *dev_client,
                                                    RedCharDeviceMsgToClient *msg)
{
    RedCharDevice *dev = dev_client->dev;
    RedCharDeviceMsgToClientItem *msg_item;

    if (dev_client->send_queue_size >= dev_client->max_send_queue_size) {
        red_char_device_handle_client_overflow(dev_client);
        return;
    }

    msg_item = spice_new0(RedCharDeviceMsgToClientItem, 1);
    msg_item->msg = red_char_device_ref_msg_to_client(dev, msg);
    ring_add(&dev_client->send_queue, &msg_item->link);
    dev_client->send_queue_size++;
    if (!dev_client->wait_for_tokens_started) {
        reds_core_timer_start(dev->priv->reds, dev_client->wait_for_tokens_timer,
                              RED_CHAR_DEVICE_WAIT_TOKENS_TIMEOUT);
        dev_client->wait_for_tokens_started = TRUE;
    }
}

static void red_char_device_send_msg_to_clients(RedCharDevice *dev,
                                                RedCharDeviceMsgToClient *msg)
{
    RingItem *item, *next;

    RING_FOREACH_SAFE(item, next, &dev->priv->clients) {
        RedCharDeviceClient *dev_client;

        dev_client = SPICE_CONTAINEROF(item, RedCharDeviceClient, link);
        if (red_char_device_can_send_to_client(dev_client)) {
            dev_client->num_send_tokens--;
            spice_assert(ring_is_empty(&dev_client->send_queue));
            red_char_device_send_msg_to_client(dev, msg, dev_client->client);

            /* don't refer to dev_client anymore, it may have been released */
        } else {
            red_char_device_add_msg_to_client_queue(dev_client, msg);
        }
    }
}

static int red_char_device_read_from_device(RedCharDevice *dev)
{
    uint64_t max_send_tokens;
    int did_read = FALSE;

    if (!dev->priv->running || dev->priv->wait_for_migrate_data || !dev->priv->sin) {
        return FALSE;
    }

    /* There are 2 scenarios where we can get called recursively:
     * 1) spice-vmc vmc_read triggering flush of throttled data, recalling wakeup
     * (virtio)
     * 2) in case of sending messages to the client, and unreferencing the
     * msg, we trigger another read.
     */
    if (dev->priv->during_read_from_device++ > 0) {
        return FALSE;
    }

    max_send_tokens = red_char_device_max_send_tokens(dev);
    red_char_device_ref(dev);
    /*
     * Reading from the device only in case at least one of the clients have a free token.
     * All messages will be discarded if no client is attached to the device
     */
    while ((max_send_tokens || ring_is_empty(&dev->priv->clients)) && dev->priv->running) {
        RedCharDeviceMsgToClient *msg;

        msg = red_char_device_read_one_msg_from_device(dev);
        if (!msg) {
            if (dev->priv->during_read_from_device > 1) {
                dev->priv->during_read_from_device = 1;
                continue; /* a wakeup might have been called during the read -
                             make sure it doesn't get lost */
            }
            break;
        }
        did_read = TRUE;
        red_char_device_send_msg_to_clients(dev, msg);
        red_char_device_unref_msg_to_client(dev, msg);
        max_send_tokens--;
    }
    dev->priv->during_read_from_device = 0;
    if (dev->priv->running) {
        dev->priv->active = dev->priv->active || did_read;
    }
    red_char_device_unref(dev);
    return did_read;
}

static void red_char_device_client_send_queue_push(RedCharDeviceClient *dev_client)
{
    RingItem *item;
    while ((item = ring_get_tail(&dev_client->send_queue)) &&
           red_char_device_can_send_to_client(dev_client)) {
        RedCharDeviceMsgToClientItem *msg_item;

        msg_item = SPICE_CONTAINEROF(item, RedCharDeviceMsgToClientItem, link);
        ring_remove(item);

        dev_client->num_send_tokens--;
        red_char_device_send_msg_to_client(dev_client->dev,
                                           msg_item->msg,
                                           dev_client->client);
        red_char_device_unref_msg_to_client(dev_client->dev, msg_item->msg);
        dev_client->send_queue_size--;
        free(msg_item);
    }
}

static void red_char_device_send_to_client_tokens_absorb(RedCharDeviceClient *dev_client,
                                                         uint32_t tokens)
{
    RedCharDevice *dev = dev_client->dev;
    dev_client->num_send_tokens += tokens;

    if (dev_client->send_queue_size) {
        spice_assert(dev_client->num_send_tokens == tokens);
        red_char_device_client_send_queue_push(dev_client);
    }

    if (red_char_device_can_send_to_client(dev_client)) {
        reds_core_timer_cancel(dev->priv->reds, dev_client->wait_for_tokens_timer);
        dev_client->wait_for_tokens_started = FALSE;
        red_char_device_read_from_device(dev_client->dev);
    } else if (dev_client->send_queue_size) {
        reds_core_timer_start(dev->priv->reds, dev_client->wait_for_tokens_timer,
                              RED_CHAR_DEVICE_WAIT_TOKENS_TIMEOUT);
        dev_client->wait_for_tokens_started = TRUE;
    }
}

void red_char_device_send_to_client_tokens_add(RedCharDevice *dev,
                                               RedClient *client,
                                               uint32_t tokens)
{
    RedCharDeviceClient *dev_client;

    dev_client = red_char_device_client_find(dev, client);

    if (!dev_client) {
        spice_error("client wasn't found dev %p client %p", dev, client);
        return;
    }
    red_char_device_send_to_client_tokens_absorb(dev_client, tokens);
}

void red_char_device_send_to_client_tokens_set(RedCharDevice *dev,
                                               RedClient *client,
                                               uint32_t tokens)
{
    RedCharDeviceClient *dev_client;

    dev_client = red_char_device_client_find(dev, client);

    if (!dev_client) {
        spice_error("client wasn't found dev %p client %p", dev, client);
        return;
    }

    dev_client->num_send_tokens = 0;
    red_char_device_send_to_client_tokens_absorb(dev_client, tokens);
}

/**************************
 * Writing to the device  *
***************************/

static void red_char_device_client_tokens_add(RedCharDevice *dev,
                                              RedCharDeviceClient *dev_client,
                                              uint32_t num_tokens)
{
    if (!dev_client->do_flow_control) {
        return;
    }
    if (num_tokens > 1) {
        spice_debug("#tokens > 1 (=%u)", num_tokens);
    }
    dev_client->num_client_tokens_free += num_tokens;
    if (dev_client->num_client_tokens_free >= dev->priv->client_tokens_interval) {
        uint32_t tokens = dev_client->num_client_tokens_free;

        dev_client->num_client_tokens += dev_client->num_client_tokens_free;
        dev_client->num_client_tokens_free = 0;
        red_char_device_send_tokens_to_client(dev, dev_client->client, tokens);
    }
}

static int red_char_device_write_to_device(RedCharDevice *dev)
{
    SpiceCharDeviceInterface *sif;
    int total = 0;
    int n;

    if (!dev->priv->running || dev->priv->wait_for_migrate_data || !dev->priv->sin) {
        return 0;
    }

    /* protect against recursion with red_char_device_wakeup */
    if (dev->priv->during_write_to_device++ > 0) {
        return 0;
    }

    red_char_device_ref(dev);

    if (dev->priv->write_to_dev_timer) {
        reds_core_timer_cancel(dev->priv->reds, dev->priv->write_to_dev_timer);
    }

    sif = spice_char_device_get_interface(dev->priv->sin);
    while (dev->priv->running) {
        uint32_t write_len;

        if (!dev->priv->cur_write_buf) {
            RingItem *item = ring_get_tail(&dev->priv->write_queue);
            if (!item) {
                break;
            }
            dev->priv->cur_write_buf = SPICE_CONTAINEROF(item, RedCharDeviceWriteBuffer, link);
            dev->priv->cur_write_buf_pos = dev->priv->cur_write_buf->buf;
            ring_remove(item);
        }

        write_len = dev->priv->cur_write_buf->buf + dev->priv->cur_write_buf->buf_used -
                    dev->priv->cur_write_buf_pos;
        n = sif->write(dev->priv->sin, dev->priv->cur_write_buf_pos, write_len);
        if (n <= 0) {
            if (dev->priv->during_write_to_device > 1) {
                dev->priv->during_write_to_device = 1;
                continue; /* a wakeup might have been called during the write -
                             make sure it doesn't get lost */
            }
            break;
        }
        total += n;
        write_len -= n;
        if (!write_len) {
            RedCharDeviceWriteBuffer *release_buf = dev->priv->cur_write_buf;
            dev->priv->cur_write_buf = NULL;
            red_char_device_write_buffer_release(dev, release_buf);
            continue;
        }
        dev->priv->cur_write_buf_pos += n;
    }
    /* retry writing as long as the write queue is not empty */
    if (dev->priv->running) {
        if (dev->priv->cur_write_buf) {
            if (dev->priv->write_to_dev_timer) {
                reds_core_timer_start(dev->priv->reds, dev->priv->write_to_dev_timer,
                                      CHAR_DEVICE_WRITE_TO_TIMEOUT);
            }
        } else {
            spice_assert(ring_is_empty(&dev->priv->write_queue));
        }
        dev->priv->active = dev->priv->active || total;
    }
    dev->priv->during_write_to_device = 0;
    red_char_device_unref(dev);
    return total;
}

static void red_char_device_write_retry(void *opaque)
{
    RedCharDevice *dev = opaque;

    if (dev->priv->write_to_dev_timer) {
        reds_core_timer_cancel(dev->priv->reds, dev->priv->write_to_dev_timer);
    }
    red_char_device_write_to_device(dev);
}

static RedCharDeviceWriteBuffer *__red_char_device_write_buffer_get(
    RedCharDevice *dev, RedClient *client,
    int size, int origin, int migrated_data_tokens)
{
    RingItem *item;
    RedCharDeviceWriteBuffer *ret;

    if (origin == WRITE_BUFFER_ORIGIN_SERVER && !dev->priv->num_self_tokens) {
        return NULL;
    }

    if ((item = ring_get_tail(&dev->priv->write_bufs_pool))) {
        ret = SPICE_CONTAINEROF(item, RedCharDeviceWriteBuffer, link);
        ring_remove(item);
        dev->priv->cur_pool_size -= ret->buf_size;
    } else {
        ret = spice_new0(RedCharDeviceWriteBuffer, 1);
    }

    spice_assert(!ret->buf_used);

    if (ret->buf_size < size) {
        ret->buf = spice_realloc(ret->buf, size);
        ret->buf_size = size;
    }
    ret->origin = origin;

    if (origin == WRITE_BUFFER_ORIGIN_CLIENT) {
       spice_assert(client);
       RedCharDeviceClient *dev_client = red_char_device_client_find(dev, client);
       if (dev_client) {
            if (!migrated_data_tokens &&
                dev_client->do_flow_control && !dev_client->num_client_tokens) {
                spice_printerr("token violation: dev %p client %p", dev, client);
                red_char_device_handle_client_overflow(dev_client);
                goto error;
            }
            ret->client = client;
            if (!migrated_data_tokens && dev_client->do_flow_control) {
                dev_client->num_client_tokens--;
            }
        } else {
            /* it is possible that the client was removed due to send tokens underflow, but
             * the caller still receive messages from the client */
            spice_printerr("client not found: dev %p client %p", dev, client);
            goto error;
        }
    } else if (origin == WRITE_BUFFER_ORIGIN_SERVER) {
        dev->priv->num_self_tokens--;
    }

    ret->token_price = migrated_data_tokens ? migrated_data_tokens : 1;
    ret->refs = 1;
    return ret;
error:
    dev->priv->cur_pool_size += ret->buf_size;
    ring_add(&dev->priv->write_bufs_pool, &ret->link);
    return NULL;
}

RedCharDeviceWriteBuffer *red_char_device_write_buffer_get(RedCharDevice *dev,
                                                           RedClient *client,
                                                           int size)
{
   return  __red_char_device_write_buffer_get(dev, client, size,
             client ? WRITE_BUFFER_ORIGIN_CLIENT : WRITE_BUFFER_ORIGIN_SERVER,
             0);
}

RedCharDeviceWriteBuffer *red_char_device_write_buffer_get_server_no_token(
    RedCharDevice *dev, int size)
{
   return  __red_char_device_write_buffer_get(dev, NULL, size,
             WRITE_BUFFER_ORIGIN_SERVER_NO_TOKEN, 0);
}

static RedCharDeviceWriteBuffer *red_char_device_write_buffer_ref(RedCharDeviceWriteBuffer *write_buf)
{
    spice_assert(write_buf);

    write_buf->refs++;
    return write_buf;
}

static void red_char_device_write_buffer_unref(RedCharDeviceWriteBuffer *write_buf)
{
    spice_assert(write_buf);

    write_buf->refs--;
    if (write_buf->refs == 0)
        red_char_device_write_buffer_free(write_buf);
}

void red_char_device_write_buffer_add(RedCharDevice *dev,
                                      RedCharDeviceWriteBuffer *write_buf)
{
    spice_assert(dev);
    /* caller shouldn't add buffers for client that was removed */
    if (write_buf->origin == WRITE_BUFFER_ORIGIN_CLIENT &&
        !red_char_device_client_find(dev, write_buf->client)) {
        spice_printerr("client not found: dev %p client %p", dev, write_buf->client);
        red_char_device_write_buffer_pool_add(dev, write_buf);
        return;
    }

    ring_add(&dev->priv->write_queue, &write_buf->link);
    red_char_device_write_to_device(dev);
}

void red_char_device_write_buffer_release(RedCharDevice *dev,
                                          RedCharDeviceWriteBuffer *write_buf)
{
    int buf_origin = write_buf->origin;
    uint32_t buf_token_price = write_buf->token_price;
    RedClient *client = write_buf->client;

    spice_assert(!ring_item_is_linked(&write_buf->link));
    if (!dev) {
        spice_printerr("no device. write buffer is freed");
        red_char_device_write_buffer_free(write_buf);
        return;
    }

    spice_assert(dev->priv->cur_write_buf != write_buf);

    red_char_device_write_buffer_pool_add(dev, write_buf);
    if (buf_origin == WRITE_BUFFER_ORIGIN_CLIENT) {
        RedCharDeviceClient *dev_client;

        spice_assert(client);
        dev_client = red_char_device_client_find(dev, client);
        /* when a client is removed, we remove all the buffers that are associated with it */
        spice_assert(dev_client);
        red_char_device_client_tokens_add(dev, dev_client, buf_token_price);
    } else if (buf_origin == WRITE_BUFFER_ORIGIN_SERVER) {
        dev->priv->num_self_tokens++;
        red_char_device_on_free_self_token(dev);
    }
}

/********************************
 * char_device_state management *
 ********************************/

RedCharDevice *red_char_device_create(SpiceCharDeviceInstance *sin,
                                      RedsState *reds,
                                      uint32_t client_tokens_interval,
                                      uint32_t self_tokens,
                                      RedCharDeviceCallbacks *cbs,
                                      void *opaque)
{
    RedCharDevice *char_dev;
    SpiceCharDeviceInterface *sif;

    spice_assert(sin);
    spice_assert(cbs->read_one_msg_from_device && cbs->ref_msg_to_client &&
                 cbs->unref_msg_to_client && cbs->send_msg_to_client &&
                 cbs->send_tokens_to_client && cbs->remove_client);

    char_dev = spice_new0(RedCharDevice, 1);
    char_dev->priv->sin = sin;
    char_dev->priv->reds = reds;
    char_dev->priv->cbs = *cbs;
    char_dev->priv->opaque = opaque;
    char_dev->priv->client_tokens_interval = client_tokens_interval;
    char_dev->priv->num_self_tokens = self_tokens;

    ring_init(&char_dev->priv->write_queue);
    ring_init(&char_dev->priv->write_bufs_pool);
    ring_init(&char_dev->priv->clients);

    sif = spice_char_device_get_interface(char_dev->priv->sin);
    if (sif->base.minor_version <= 2 ||
        !(sif->flags & SPICE_CHAR_DEVICE_NOTIFY_WRITABLE)) {
        char_dev->priv->write_to_dev_timer = reds_core_timer_add(reds, red_char_device_write_retry, char_dev);
        if (!char_dev->priv->write_to_dev_timer) {
            spice_error("failed creating char dev write timer");
        }
    }

    char_dev->priv->refs = 1;
    sin->st = char_dev;
    spice_debug("sin %p dev_state %p", sin, char_dev);
    return char_dev;
}

void red_char_device_reset_dev_instance(RedCharDevice *state,
                                        SpiceCharDeviceInstance *sin)
{
    spice_debug("sin %p dev_state %p", sin, state);
    state->priv->sin = sin;
    sin->st = state;
}

void *red_char_device_opaque_get(RedCharDevice *dev)
{
    return dev->priv->opaque;
}

static void red_char_device_ref(RedCharDevice *char_dev)
{
    char_dev->priv->refs++;
}

static void red_char_device_unref(RedCharDevice *char_dev)
{
    /* The refs field protects the char_dev from being deallocated in
     * case red_char_device_destroy has been called
     * during a callabck, and we might still access the char_dev afterwards.
     * red_char_device_unref is always coupled with a preceding
     * red_char_device_ref. Here, refs can turn 0
     * only when red_char_device_destroy is called in between
     * the calls to red_char_device_ref and red_char_device_unref.*/
    if (!--char_dev->priv->refs) {
        free(char_dev);
    }
}

void red_char_device_destroy(RedCharDevice *char_dev)
{
    reds_on_char_device_state_destroy(char_dev->priv->reds, char_dev);
    if (char_dev->priv->write_to_dev_timer) {
        reds_core_timer_remove(char_dev->priv->reds, char_dev->priv->write_to_dev_timer);
        char_dev->priv->write_to_dev_timer = NULL;
    }
    write_buffers_queue_free(&char_dev->priv->write_queue);
    write_buffers_queue_free(&char_dev->priv->write_bufs_pool);
    char_dev->priv->cur_pool_size = 0;
    red_char_device_write_buffer_free(char_dev->priv->cur_write_buf);
    char_dev->priv->cur_write_buf = NULL;

    while (!ring_is_empty(&char_dev->priv->clients)) {
        RingItem *item = ring_get_tail(&char_dev->priv->clients);
        RedCharDeviceClient *dev_client;

        dev_client = SPICE_CONTAINEROF(item, RedCharDeviceClient, link);
        red_char_device_client_free(char_dev, dev_client);
    }
    char_dev->priv->running = FALSE;

    red_char_device_unref(char_dev);
}

static RedCharDeviceClient *red_char_device_client_new(RedClient *client,
                                                       int do_flow_control,
                                                       uint32_t max_send_queue_size,
                                                       uint32_t num_client_tokens,
                                                       uint32_t num_send_tokens)
{
    RedCharDeviceClient *dev_client;

    dev_client = spice_new0(RedCharDeviceClient, 1);
    dev_client->client = client;
    ring_init(&dev_client->send_queue);
    dev_client->send_queue_size = 0;
    dev_client->max_send_queue_size = max_send_queue_size;
    dev_client->do_flow_control = do_flow_control;
    if (do_flow_control) {
        dev_client->wait_for_tokens_timer =
            reds_core_timer_add(client->reds, device_client_wait_for_tokens_timeout,
                                dev_client);
        if (!dev_client->wait_for_tokens_timer) {
            spice_error("failed to create wait for tokens timer");
        }
        dev_client->num_client_tokens = num_client_tokens;
        dev_client->num_send_tokens = num_send_tokens;
    } else {
        dev_client->num_client_tokens = ~0;
        dev_client->num_send_tokens = ~0;
    }

    return dev_client;
}

int red_char_device_client_add(RedCharDevice *dev,
                               RedClient *client,
                               int do_flow_control,
                               uint32_t max_send_queue_size,
                               uint32_t num_client_tokens,
                               uint32_t num_send_tokens,
                               int wait_for_migrate_data)
{
    RedCharDeviceClient *dev_client;

    spice_assert(dev);
    spice_assert(client);

    if (wait_for_migrate_data && (dev->priv->num_clients > 0 || dev->priv->active)) {
        spice_warning("can't restore device %p from migration data. The device "
                      "has already been active", dev);
        return FALSE;
    }

    dev->priv->wait_for_migrate_data = wait_for_migrate_data;

    spice_debug("dev_state %p client %p", dev, client);
    dev_client = red_char_device_client_new(client, do_flow_control,
                                            max_send_queue_size,
                                            num_client_tokens,
                                            num_send_tokens);
    dev_client->dev = dev;
    ring_add(&dev->priv->clients, &dev_client->link);
    dev->priv->num_clients++;
    /* Now that we have a client, forward any pending device data */
    red_char_device_wakeup(dev);
    return TRUE;
}

void red_char_device_client_remove(RedCharDevice *dev,
                                   RedClient *client)
{
    RedCharDeviceClient *dev_client;

    spice_debug("dev_state %p client %p", dev, client);
    dev_client = red_char_device_client_find(dev, client);

    if (!dev_client) {
        spice_error("client wasn't found");
        return;
    }
    red_char_device_client_free(dev, dev_client);
    if (dev->priv->wait_for_migrate_data) {
        spice_assert(dev->priv->num_clients == 0);
        dev->priv->wait_for_migrate_data  = FALSE;
        red_char_device_read_from_device(dev);
    }

    if (dev->priv->num_clients == 0) {
        spice_debug("client removed, memory pool will be freed (%"PRIu64" bytes)", dev->priv->cur_pool_size);
        write_buffers_queue_free(&dev->priv->write_bufs_pool);
        dev->priv->cur_pool_size = 0;
    }
}

int red_char_device_client_exists(RedCharDevice *dev,
                                  RedClient *client)
{
    return (red_char_device_client_find(dev, client) != NULL);
}

void red_char_device_start(RedCharDevice *dev)
{
    spice_debug("dev_state %p", dev);
    dev->priv->running = TRUE;
    red_char_device_ref(dev);
    while (red_char_device_write_to_device(dev) ||
           red_char_device_read_from_device(dev));
    red_char_device_unref(dev);
}

void red_char_device_stop(RedCharDevice *dev)
{
    spice_debug("dev_state %p", dev);
    dev->priv->running = FALSE;
    dev->priv->active = FALSE;
    if (dev->priv->write_to_dev_timer) {
        reds_core_timer_cancel(dev->priv->reds, dev->priv->write_to_dev_timer);
    }
}

void red_char_device_reset(RedCharDevice *dev)
{
    RingItem *client_item;

    red_char_device_stop(dev);
    dev->priv->wait_for_migrate_data = FALSE;
    spice_debug("dev_state %p", dev);
    while (!ring_is_empty(&dev->priv->write_queue)) {
        RingItem *item = ring_get_tail(&dev->priv->write_queue);
        RedCharDeviceWriteBuffer *buf;

        ring_remove(item);
        buf = SPICE_CONTAINEROF(item, RedCharDeviceWriteBuffer, link);
        /* tracking the tokens */
        red_char_device_write_buffer_release(dev, buf);
    }
    if (dev->priv->cur_write_buf) {
        RedCharDeviceWriteBuffer *release_buf = dev->priv->cur_write_buf;

        dev->priv->cur_write_buf = NULL;
        red_char_device_write_buffer_release(dev, release_buf);
    }

    RING_FOREACH(client_item, &dev->priv->clients) {
        RedCharDeviceClient *dev_client;

        dev_client = SPICE_CONTAINEROF(client_item, RedCharDeviceClient, link);
        red_char_device_client_send_queue_free(dev, dev_client);
    }
    dev->priv->sin = NULL;
}

void red_char_device_wakeup(RedCharDevice *dev)
{
    red_char_device_write_to_device(dev);
    red_char_device_read_from_device(dev);
}

/*************
 * Migration *
 * **********/

void red_char_device_migrate_data_marshall_empty(SpiceMarshaller *m)
{
    SpiceMigrateDataCharDevice *mig_data;

    spice_debug(NULL);
    mig_data = (SpiceMigrateDataCharDevice *)spice_marshaller_reserve_space(m,
                                                                            sizeof(*mig_data));
    memset(mig_data, 0, sizeof(*mig_data));
    mig_data->version = SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION;
    mig_data->connected = FALSE;
}

static void migrate_data_marshaller_write_buffer_free(uint8_t *data, void *opaque)
{
    RedCharDeviceWriteBuffer *write_buf = (RedCharDeviceWriteBuffer *)opaque;

    red_char_device_write_buffer_unref(write_buf);
}

void red_char_device_migrate_data_marshall(RedCharDevice *dev,
                                           SpiceMarshaller *m)
{
    RedCharDeviceClient *dev_client;
    RingItem *item;
    uint32_t *write_to_dev_size_ptr;
    uint32_t *write_to_dev_tokens_ptr;
    SpiceMarshaller *m2;

    /* multi-clients are not supported */
    spice_assert(dev->priv->num_clients == 1);
    dev_client = SPICE_CONTAINEROF(ring_get_tail(&dev->priv->clients),
                                   RedCharDeviceClient,
                                   link);
    /* FIXME: if there were more than one client before the marshalling,
     * it is possible that the send_queue_size > 0, and the send data
     * should be migrated as well */
    spice_assert(dev_client->send_queue_size == 0);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION);
    spice_marshaller_add_uint8(m, 1); /* connected */
    spice_marshaller_add_uint32(m, dev_client->num_client_tokens);
    spice_marshaller_add_uint32(m, dev_client->num_send_tokens);
    write_to_dev_size_ptr = (uint32_t *)spice_marshaller_reserve_space(m, sizeof(uint32_t));
    write_to_dev_tokens_ptr = (uint32_t *)spice_marshaller_reserve_space(m, sizeof(uint32_t));
    *write_to_dev_size_ptr = 0;
    *write_to_dev_tokens_ptr = 0;

    m2 = spice_marshaller_get_ptr_submarshaller(m, 0);
    if (dev->priv->cur_write_buf) {
        uint32_t buf_remaining = dev->priv->cur_write_buf->buf + dev->priv->cur_write_buf->buf_used -
                                 dev->priv->cur_write_buf_pos;
        spice_marshaller_add_ref_full(m2, dev->priv->cur_write_buf_pos, buf_remaining,
                                      migrate_data_marshaller_write_buffer_free,
                                      red_char_device_write_buffer_ref(dev->priv->cur_write_buf)
                                      );
        *write_to_dev_size_ptr += buf_remaining;
        if (dev->priv->cur_write_buf->origin == WRITE_BUFFER_ORIGIN_CLIENT) {
            spice_assert(dev->priv->cur_write_buf->client == dev_client->client);
            (*write_to_dev_tokens_ptr) += dev->priv->cur_write_buf->token_price;
        }
    }

    RING_FOREACH_REVERSED(item, &dev->priv->write_queue) {
        RedCharDeviceWriteBuffer *write_buf;

        write_buf = SPICE_CONTAINEROF(item, RedCharDeviceWriteBuffer, link);
        spice_marshaller_add_ref_full(m2, write_buf->buf, write_buf->buf_used,
                                      migrate_data_marshaller_write_buffer_free,
                                      red_char_device_write_buffer_ref(write_buf)
                                      );
        *write_to_dev_size_ptr += write_buf->buf_used;
        if (write_buf->origin == WRITE_BUFFER_ORIGIN_CLIENT) {
            spice_assert(write_buf->client == dev_client->client);
            (*write_to_dev_tokens_ptr) += write_buf->token_price;
        }
    }
    spice_debug("migration data dev %p: write_queue size %u tokens %u",
                dev, *write_to_dev_size_ptr, *write_to_dev_tokens_ptr);
}

int red_char_device_restore(RedCharDevice *dev,
                            SpiceMigrateDataCharDevice *mig_data)
{
    RedCharDeviceClient *dev_client;
    uint32_t client_tokens_window;

    spice_assert(dev->priv->num_clients == 1 && dev->priv->wait_for_migrate_data);

    dev_client = SPICE_CONTAINEROF(ring_get_tail(&dev->priv->clients),
                                     RedCharDeviceClient,
                                     link);
    if (mig_data->version > SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION) {
        spice_error("dev %p error: migration data version %u is bigger than self %u",
                    dev, mig_data->version, SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION);
        return FALSE;
    }
    spice_assert(!dev->priv->cur_write_buf && ring_is_empty(&dev->priv->write_queue));
    spice_assert(mig_data->connected);

    client_tokens_window = dev_client->num_client_tokens; /* initial state of tokens */
    dev_client->num_client_tokens = mig_data->num_client_tokens;
    /* assumption: client_tokens_window stays the same across severs */
    dev_client->num_client_tokens_free = client_tokens_window -
                                           mig_data->num_client_tokens -
                                           mig_data->write_num_client_tokens;
    dev_client->num_send_tokens = mig_data->num_send_tokens;

    if (mig_data->write_size > 0) {
        if (mig_data->write_num_client_tokens) {
            dev->priv->cur_write_buf =
                __red_char_device_write_buffer_get(dev, dev_client->client,
                    mig_data->write_size, WRITE_BUFFER_ORIGIN_CLIENT,
                    mig_data->write_num_client_tokens);
        } else {
            dev->priv->cur_write_buf =
                __red_char_device_write_buffer_get(dev, NULL,
                    mig_data->write_size, WRITE_BUFFER_ORIGIN_SERVER, 0);
        }
        /* the first write buffer contains all the data that was saved for migration */
        memcpy(dev->priv->cur_write_buf->buf,
               ((uint8_t *)mig_data) + mig_data->write_data_ptr - sizeof(SpiceMigrateDataHeader),
               mig_data->write_size);
        dev->priv->cur_write_buf->buf_used = mig_data->write_size;
        dev->priv->cur_write_buf_pos = dev->priv->cur_write_buf->buf;
    }
    dev->priv->wait_for_migrate_data = FALSE;
    red_char_device_write_to_device(dev);
    red_char_device_read_from_device(dev);
    return TRUE;
}

SpiceServer* red_char_device_get_server(RedCharDevice *dev)
{
    return dev->priv->reds;
}

SpiceCharDeviceInterface *spice_char_device_get_interface(SpiceCharDeviceInstance *instance)
{
   return SPICE_CONTAINEROF(instance->base.sif, SpiceCharDeviceInterface, base);
}
