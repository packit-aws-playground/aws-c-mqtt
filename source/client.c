/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/mqtt/client.h>

#include <aws/mqtt/private/client_impl.h>
#include <aws/mqtt/private/mqtt_client_test_helper.h>
#include <aws/mqtt/private/packets.h>
#include <aws/mqtt/private/topic_tree.h>

#include <aws/http/proxy.h>

#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/logging.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>

#include <aws/common/clock.h>
#include <aws/common/task_scheduler.h>

#include <inttypes.h>

#ifdef AWS_MQTT_WITH_WEBSOCKETS
#    include <aws/http/connection.h>
#    include <aws/http/request_response.h>
#    include <aws/http/websocket.h>
#endif

#ifdef _MSC_VER
#    pragma warning(disable : 4204)
#endif

/* 3 seconds */
static const uint64_t s_default_ping_timeout_ns = 3000000000;

/* 20 minutes - This is the default (and max) for AWS IoT as of 2020.02.18 */
static const uint16_t s_default_keep_alive_sec = 1200;

static int s_mqtt_client_connect(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_connection_complete_fn *on_connection_complete,
    void *userdata);
/*******************************************************************************
 * Helper functions
 ******************************************************************************/

void mqtt_connection_lock_synced_data(struct aws_mqtt_client_connection *connection) {
    int err = aws_mutex_lock(&connection->synced_data.lock);
    AWS_ASSERT(!err);
    (void)err;
}

void mqtt_connection_unlock_synced_data(struct aws_mqtt_client_connection *connection) {
    ASSERT_SYNCED_DATA_LOCK_HELD(connection);

    int err = aws_mutex_unlock(&connection->synced_data.lock);
    AWS_ASSERT(!err);
    (void)err;
}

static void s_aws_mqtt_client_destroy(struct aws_mqtt_client *client) {

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "client=%p: Cleaning up MQTT client", (void *)client);
    aws_client_bootstrap_release(client->bootstrap);

    aws_mem_release(client->allocator, client);
}

void mqtt_connection_set_state(
    struct aws_mqtt_client_connection *connection,
    enum aws_mqtt_client_connection_state state) {
    ASSERT_SYNCED_DATA_LOCK_HELD(connection);
    if (connection->synced_data.state == state) {
        AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: MQTT connection already in state %d", (void *)connection, state);
        return;
    }
    connection->synced_data.state = state;
}

struct request_timeout_wrapper;

/* used for timeout task */
struct request_timeout_task_arg {
    uint16_t packet_id;
    struct aws_mqtt_client_connection *connection;
    struct request_timeout_wrapper *task_arg_wrapper;
};

/*
 * We want the timeout task to be able to destroy the forward reference from the operation's task arg structure
 * to the timeout task.  But the operation task arg structures don't have any data structure in common.  So to allow
 * the timeout to refer back to a zero-able forward pointer, we wrap a pointer to the timeout task and embed it
 * in every operation's task arg that needs to create a timeout.
 */
struct request_timeout_wrapper {
    struct request_timeout_task_arg *timeout_task_arg;
};

static void s_request_timeout(struct aws_channel_task *channel_task, void *arg, enum aws_task_status status) {
    (void)channel_task;
    struct request_timeout_task_arg *timeout_task_arg = arg;
    struct aws_mqtt_client_connection *connection = timeout_task_arg->connection;

    if (status == AWS_TASK_STATUS_RUN_READY) {
        if (timeout_task_arg->task_arg_wrapper != NULL) {
            mqtt_request_complete(connection, AWS_ERROR_MQTT_TIMEOUT, timeout_task_arg->packet_id);
        }
    }

    /*
     * Whether cancelled or run, if we have a back pointer to the operation's task arg, we must zero it out
     * so that when it completes it does not try to cancel us, because we will already be freed.
     *
     * If we don't have a back pointer to the operation's task arg, that means it already ran and completed.
     */
    if (timeout_task_arg->task_arg_wrapper != NULL) {
        timeout_task_arg->task_arg_wrapper->timeout_task_arg = NULL;
        timeout_task_arg->task_arg_wrapper = NULL;
    }

    aws_mem_release(connection->allocator, timeout_task_arg);
}

static struct request_timeout_task_arg *s_schedule_timeout_task(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id) {
    /* schedule a timeout task to run, in case server consider the publish is not received */
    struct aws_channel_task *request_timeout_task = NULL;
    struct request_timeout_task_arg *timeout_task_arg = NULL;
    if (!aws_mem_acquire_many(
            connection->allocator,
            2,
            &timeout_task_arg,
            sizeof(struct request_timeout_task_arg),
            &request_timeout_task,
            sizeof(struct aws_channel_task))) {
        return NULL;
    }
    aws_channel_task_init(request_timeout_task, s_request_timeout, timeout_task_arg, "mqtt_request_timeout");
    AWS_ZERO_STRUCT(*timeout_task_arg);
    timeout_task_arg->connection = connection;
    timeout_task_arg->packet_id = packet_id;
    uint64_t timestamp = 0;
    if (aws_channel_current_clock_time(connection->slot->channel, &timestamp)) {
        aws_mem_release(connection->allocator, timeout_task_arg);
        return NULL;
    }
    timestamp = aws_add_u64_saturating(timestamp, connection->operation_timeout_ns);
    aws_channel_schedule_task_future(connection->slot->channel, request_timeout_task, timestamp);
    return timeout_task_arg;
}

/*******************************************************************************
 * Client Init
 ******************************************************************************/
struct aws_mqtt_client *aws_mqtt_client_new(struct aws_allocator *allocator, struct aws_client_bootstrap *bootstrap) {

    aws_mqtt_fatal_assert_library_initialized();

    struct aws_mqtt_client *client = aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_client));
    if (client == NULL) {
        return NULL;
    }

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "client=%p: Initalizing MQTT client", (void *)client);

    client->allocator = allocator;
    client->bootstrap = aws_client_bootstrap_acquire(bootstrap);
    aws_ref_count_init(&client->ref_count, client, (aws_simple_completion_callback *)s_aws_mqtt_client_destroy);

    return client;
}

struct aws_mqtt_client *aws_mqtt_client_acquire(struct aws_mqtt_client *client) {
    if (client != NULL) {
        aws_ref_count_acquire(&client->ref_count);
    }

    return client;
}

void aws_mqtt_client_release(struct aws_mqtt_client *client) {
    if (client != NULL) {
        aws_ref_count_release(&client->ref_count);
    }
}

/* At this point, the channel for the MQTT connection has completed its shutdown */
static void s_mqtt_client_shutdown(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    (void)channel;

    struct aws_mqtt_client_connection *connection = user_data;

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT, "id=%p: Channel has been shutdown with error code %d", (void *)connection, error_code);
    enum aws_mqtt_client_connection_state prev_state;
    struct aws_linked_list cancelling_requests;
    aws_linked_list_init(&cancelling_requests);
    bool disconnected_state = false;
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);
        /* Move all the ongoing requests to the pending requests list, because the response they are waiting for will
         * never arrives. Sad. But, we will retry. */
        if (connection->clean_session) {
            /* For a clean session, the Session lasts as long as the Network Connection. Thus, discard the previous
             * session */
            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT,
                "id=%p: Discard ongoing requests and pending requests when a clean session connection lost.",
                (void *)connection);
            aws_linked_list_move_all_back(&cancelling_requests, &connection->thread_data.ongoing_requests_list);
            aws_linked_list_move_all_back(&cancelling_requests, &connection->synced_data.pending_requests_list);
        } else {
            aws_linked_list_move_all_back(
                &connection->synced_data.pending_requests_list, &connection->thread_data.ongoing_requests_list);
            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT,
                "id=%p: All subscribe/unsubscribe and publish QoS>0 have been move to pending list",
                (void *)connection);
        }
        prev_state = connection->synced_data.state;
        switch (connection->synced_data.state) {
            case AWS_MQTT_CLIENT_STATE_CONNECTED:
                /* unexpected hangup from broker, try to reconnect */
                mqtt_connection_set_state(connection, AWS_MQTT_CLIENT_STATE_RECONNECTING);
                AWS_LOGF_DEBUG(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: connection was unexpected interrupted, switch state to RECONNECTING.",
                    (void *)connection);
                break;
            case AWS_MQTT_CLIENT_STATE_DISCONNECTING:
                /* disconnect requested by user */
                /* Successfully shutdown, if cleansession is set, ongoing and pending requests will be cleared */
                disconnected_state = true;
                AWS_LOGF_DEBUG(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: disconnect finished, switch state to DISCONNECTED.",
                    (void *)connection);
                break;
            case AWS_MQTT_CLIENT_STATE_CONNECTING:
                /* failed to connect */
                disconnected_state = true;
                break;
            case AWS_MQTT_CLIENT_STATE_RECONNECTING:
                /* reconnect failed, schedule the next attempt later, no need to change the state. */
                break;
            default:
                /* AWS_MQTT_CLIENT_STATE_DISCONNECTED */
                break;
        }
        AWS_LOGF_TRACE(
            AWS_LS_MQTT_CLIENT, "id=%p: current state is %d", (void *)connection, (int)connection->synced_data.state);
        /* Always clear slot, as that's what's been shutdown */
        if (connection->slot) {
            aws_channel_slot_remove(connection->slot);
            AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: slot is removed successfully", (void *)connection);
            connection->slot = NULL;
        }

        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (!aws_linked_list_empty(&cancelling_requests)) {
        struct aws_linked_list_node *current = aws_linked_list_front(&cancelling_requests);
        const struct aws_linked_list_node *end = aws_linked_list_end(&cancelling_requests);
        while (current != end) {
            struct aws_mqtt_request *request = AWS_CONTAINER_OF(current, struct aws_mqtt_request, list_node);
            if (request->on_complete) {
                request->on_complete(
                    connection,
                    request->packet_id,
                    AWS_ERROR_MQTT_CANCELLED_FOR_CLEAN_SESSION,
                    request->on_complete_ud);
            }
            current = current->next;
        }
        { /* BEGIN CRITICAL SECTION */
            mqtt_connection_lock_synced_data(connection);
            while (!aws_linked_list_empty(&cancelling_requests)) {
                struct aws_linked_list_node *node = aws_linked_list_pop_front(&cancelling_requests);
                struct aws_mqtt_request *request = AWS_CONTAINER_OF(node, struct aws_mqtt_request, list_node);
                aws_hash_table_remove(
                    &connection->synced_data.outstanding_requests_table, &request->packet_id, NULL, NULL);
                aws_memory_pool_release(&connection->synced_data.requests_pool, request);
            }
            mqtt_connection_unlock_synced_data(connection);
        } /* END CRITICAL SECTION */
    }

    /* If there's no error code and this wasn't user-requested, set the error code to something useful */
    if (error_code == AWS_ERROR_SUCCESS) {
        if (prev_state != AWS_MQTT_CLIENT_STATE_DISCONNECTING && prev_state != AWS_MQTT_CLIENT_STATE_DISCONNECTED) {
            error_code = AWS_ERROR_MQTT_UNEXPECTED_HANGUP;
        }
    }
    switch (prev_state) {
        case AWS_MQTT_CLIENT_STATE_RECONNECTING: {
            /* If reconnect attempt failed, schedule the next attempt */
            struct aws_event_loop *el =
                aws_event_loop_group_get_next_loop(connection->client->bootstrap->event_loop_group);

            AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Reconnect failed, retrying", (void *)connection);

            aws_event_loop_schedule_task_future(
                el, &connection->reconnect_task->task, connection->reconnect_timeouts.next_attempt_ms);
            break;
        }
        case AWS_MQTT_CLIENT_STATE_CONNECTED: {
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT_CLIENT,
                "id=%p: Connection interrupted, calling callback and attempting reconnect",
                (void *)connection);
            MQTT_CLIENT_CALL_CALLBACK_ARGS(connection, on_interrupted, error_code);

            /* In case user called disconnect from the on_interrupted callback */
            bool stop_reconnect;
            { /* BEGIN CRITICAL SECTION */
                mqtt_connection_lock_synced_data(connection);
                stop_reconnect = connection->synced_data.state == AWS_MQTT_CLIENT_STATE_DISCONNECTING;
                if (stop_reconnect) {
                    disconnected_state = true;
                    AWS_LOGF_DEBUG(
                        AWS_LS_MQTT_CLIENT,
                        "id=%p: disconnect finished, switch state to DISCONNECTED.",
                        (void *)connection);
                }
                mqtt_connection_unlock_synced_data(connection);
            } /* END CRITICAL SECTION */

            if (!stop_reconnect) {
                struct aws_event_loop *el =
                    aws_event_loop_group_get_next_loop(connection->client->bootstrap->event_loop_group);
                aws_event_loop_schedule_task_future(
                    el, &connection->reconnect_task->task, connection->reconnect_timeouts.next_attempt_ms);
            }
            break;
        }
        default:
            break;
    }
    if (disconnected_state) {
        { /* BEGIN CRITICAL SECTION */
            mqtt_connection_lock_synced_data(connection);
            mqtt_connection_set_state(connection, AWS_MQTT_CLIENT_STATE_DISCONNECTED);
            mqtt_connection_unlock_synced_data(connection);
        } /* END CRITICAL SECTION */
        switch (prev_state) {
            case AWS_MQTT_CLIENT_STATE_CONNECTED:
                AWS_LOGF_TRACE(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: Caller requested disconnect from on_interrupted callback, aborting reconnect",
                    (void *)connection);
                MQTT_CLIENT_CALL_CALLBACK(connection, on_disconnect);
                break;
            case AWS_MQTT_CLIENT_STATE_DISCONNECTING:
                AWS_LOGF_DEBUG(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: Disconnect completed, clearing request queue and calling callback",
                    (void *)connection);
                MQTT_CLIENT_CALL_CALLBACK(connection, on_disconnect);
                break;
            case AWS_MQTT_CLIENT_STATE_CONNECTING:
                AWS_LOGF_TRACE(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: Initial connection attempt failed, calling callback",
                    (void *)connection);
                MQTT_CLIENT_CALL_CALLBACK_ARGS(connection, on_connection_complete, error_code, 0, false);
                break;
            default:
                break;
        }
        /* The connection can die now. Release the refcount */
        aws_mqtt_client_connection_release(connection);
    }
}

/*******************************************************************************
 * Connection New
 ******************************************************************************/
/* The assumption here is that a connection always outlives its channels, and the channel this task was scheduled on
 * always outlives this task, so all we need to do is check the connection state. If we are in a state that waits
 * for a CONNACK, kill it off. In the case that the connection died between scheduling this task and it being executed
 * the status will always be CANCELED because this task will be canceled when the owning channel goes away. */
static void s_connack_received_timeout(struct aws_channel_task *channel_task, void *arg, enum aws_task_status status) {
    struct aws_mqtt_client_connection *connection = arg;

    if (status == AWS_TASK_STATUS_RUN_READY) {
        bool time_out = false;
        { /* BEGIN CRITICAL SECTION */
            mqtt_connection_lock_synced_data(connection);
            time_out =
                (connection->synced_data.state == AWS_MQTT_CLIENT_STATE_CONNECTING ||
                 connection->synced_data.state == AWS_MQTT_CLIENT_STATE_RECONNECTING);
            mqtt_connection_unlock_synced_data(connection);
        } /* END CRITICAL SECTION */
        if (time_out) {
            AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: mqtt CONNACK response timeout detected", (void *)connection);
            aws_channel_shutdown(connection->slot->channel, AWS_ERROR_MQTT_TIMEOUT);
        }
    }

    aws_mem_release(connection->allocator, channel_task);
}

/**
 * Channel has been initialized callback. Sets up channel handler and sends out CONNECT packet.
 * The on_connack callback is called with the CONNACK packet is received from the server.
 */
static void s_mqtt_client_init(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    struct aws_io_message *message = NULL;

    /* Setup callback contract is: if error_code is non-zero then channel is NULL. */
    AWS_FATAL_ASSERT((error_code != 0) == (channel == NULL));

    struct aws_mqtt_client_connection *connection = user_data;

    if (error_code != AWS_OP_SUCCESS) {
        /* client shutdown already handles this case, so just call that. */
        s_mqtt_client_shutdown(bootstrap, error_code, channel, user_data);
        return;
    }

    /* user requested disconnect before the channel has been set up. Stop installing the slot and sending CONNECT. */
    bool failed_create_slot = false;

    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);

        if (connection->synced_data.state == AWS_MQTT_CLIENT_STATE_DISCONNECTING) {
            /* It only happens when the user request disconnect during reconnecting, we don't need to fire any callback.
             * The on_disconnect will be invoked as channel finish shutting down. */
            mqtt_connection_unlock_synced_data(connection);
            aws_channel_shutdown(channel, AWS_ERROR_SUCCESS);
            return;
        }
        /* Create the slot */
        connection->slot = aws_channel_slot_new(channel);
        if (!connection->slot) {
            failed_create_slot = true;
        }
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    /* intall the slot and handler */
    if (failed_create_slot) {

        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to create new slot, something has gone horribly wrong, error %d (%s).",
            (void *)connection,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto handle_error;
    }

    if (aws_channel_slot_insert_end(channel, connection->slot)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to insert slot into channel %p, error %d (%s).",
            (void *)connection,
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto handle_error;
    }

    if (aws_channel_slot_set_handler(connection->slot, &connection->handler)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to set MQTT handler into slot on channel %p, error %d (%s).",
            (void *)connection,
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto handle_error;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT, "id=%p: Connection successfully opened, sending CONNECT packet", (void *)connection);

    struct aws_channel_task *connack_task = aws_mem_calloc(connection->allocator, 1, sizeof(struct aws_channel_task));
    if (!connack_task) {
        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to allocate timeout task.", (void *)connection);
        goto handle_error;
    }

    aws_channel_task_init(connack_task, s_connack_received_timeout, connection, "mqtt_connack_timeout");

    uint64_t now = 0;
    if (aws_channel_current_clock_time(channel, &now)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "static: Failed to setting MQTT handler into slot on channel %p, error %d (%s).",
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto handle_error;
    }
    now += connection->ping_timeout_ns;
    aws_channel_schedule_task_future(channel, connack_task, now);

    /* Send the connect packet */
    struct aws_mqtt_packet_connect connect;
    aws_mqtt_packet_connect_init(
        &connect,
        aws_byte_cursor_from_buf(&connection->client_id),
        connection->clean_session,
        connection->keep_alive_time_secs);

    if (connection->will.topic.buffer) {
        /* Add will if present */

        struct aws_byte_cursor topic_cur = aws_byte_cursor_from_buf(&connection->will.topic);
        struct aws_byte_cursor payload_cur = aws_byte_cursor_from_buf(&connection->will.payload);

        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Adding will to connection on " PRInSTR " with payload " PRInSTR,
            (void *)connection,
            AWS_BYTE_CURSOR_PRI(topic_cur),
            AWS_BYTE_CURSOR_PRI(payload_cur));
        aws_mqtt_packet_connect_add_will(
            &connect, topic_cur, connection->will.qos, connection->will.retain, payload_cur);
    }

    if (connection->username) {
        struct aws_byte_cursor username_cur = aws_byte_cursor_from_string(connection->username);

        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Adding username " PRInSTR " to connection",
            (void *)connection,
            AWS_BYTE_CURSOR_PRI(username_cur))

        struct aws_byte_cursor password_cur = {
            .ptr = NULL,
            .len = 0,
        };

        if (connection->password) {
            password_cur = aws_byte_cursor_from_string(connection->password);
        }

        aws_mqtt_packet_connect_add_credentials(&connect, username_cur, password_cur);
    }

    message = mqtt_get_message_for_packet(connection, &connect.fixed_header);
    if (!message) {

        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to get message from pool", (void *)connection);
        goto handle_error;
    }

    if (aws_mqtt_packet_connect_encode(&message->message_data, &connect)) {

        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to encode CONNECT packet", (void *)connection);
        goto handle_error;
    }

    if (aws_channel_slot_send_message(connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {

        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to send encoded CONNECT packet upstream", (void *)connection);
        goto handle_error;
    }

    return;

handle_error:
    MQTT_CLIENT_CALL_CALLBACK_ARGS(connection, on_connection_complete, aws_last_error(), 0, false);
    aws_channel_shutdown(channel, aws_last_error());

    if (message) {
        aws_mem_release(message->allocator, message);
    }
}

static void s_attempt_reconnect(struct aws_task *task, void *userdata, enum aws_task_status status) {

    (void)task;

    struct aws_mqtt_reconnect_task *reconnect = userdata;
    struct aws_mqtt_client_connection *connection = aws_atomic_load_ptr(&reconnect->connection_ptr);

    if (status == AWS_TASK_STATUS_RUN_READY && connection) {
        /* If the task is not cancelled and a connection has not succeeded, attempt reconnect */

        aws_high_res_clock_get_ticks(&connection->reconnect_timeouts.next_attempt_ms);
        connection->reconnect_timeouts.next_attempt_ms += aws_timestamp_convert(
            connection->reconnect_timeouts.current_sec, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);

        AWS_LOGF_TRACE(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Attempting reconnect, if it fails next attempt will be in %" PRIu64 " seconds",
            (void *)connection,
            connection->reconnect_timeouts.current_sec);

        /* Check before multiplying to avoid potential overflow */
        if (connection->reconnect_timeouts.current_sec > connection->reconnect_timeouts.max_sec / 2) {
            connection->reconnect_timeouts.current_sec = connection->reconnect_timeouts.max_sec;
        } else {
            connection->reconnect_timeouts.current_sec *= 2;
        }

        /* Apply updated reconnect_timeout to next_attempt_reset_timer_ns to prevent premature reset to min
         * of min reconnect on a successful connect after a prolonged period of failed connections */
        uint64_t now = 0;
        aws_high_res_clock_get_ticks(&now);
        connection->reconnect_timeouts.next_attempt_reset_timer_ns =
            now + 10000000000 +
            aws_timestamp_convert(
                connection->reconnect_timeouts.current_sec, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);

        if (s_mqtt_client_connect(
                connection, connection->on_connection_complete, connection->on_connection_complete_ud)) {
            /* If reconnect attempt failed, schedule the next attempt */
            struct aws_event_loop *el =
                aws_event_loop_group_get_next_loop(connection->client->bootstrap->event_loop_group);
            aws_event_loop_schedule_task_future(
                el, &connection->reconnect_task->task, connection->reconnect_timeouts.next_attempt_ms);
            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT,
                "id=%p: Scheduling reconnect, for %" PRIu64 " on event-loop %p",
                (void *)connection,
                connection->reconnect_timeouts.next_attempt_ms,
                (void *)el);
        } else {
            connection->reconnect_task->task.timestamp = 0;
        }
    } else {
        aws_mem_release(reconnect->allocator, reconnect);
    }
}

void aws_create_reconnect_task(struct aws_mqtt_client_connection *connection) {
    if (connection->reconnect_task == NULL) {
        connection->reconnect_task = aws_mem_calloc(connection->allocator, 1, sizeof(struct aws_mqtt_reconnect_task));
        AWS_FATAL_ASSERT(connection->reconnect_task != NULL);

        aws_atomic_init_ptr(&connection->reconnect_task->connection_ptr, connection);
        connection->reconnect_task->allocator = connection->allocator;
        aws_task_init(
            &connection->reconnect_task->task, s_attempt_reconnect, connection->reconnect_task, "mqtt_reconnect");
    }
}

static uint64_t s_hash_uint16_t(const void *item) {
    return *(uint16_t *)item;
}

static bool s_uint16_t_eq(const void *a, const void *b) {
    return *(uint16_t *)a == *(uint16_t *)b;
}

static void s_mqtt_client_connection_destroy_final(struct aws_mqtt_client_connection *connection) {
    AWS_PRECONDITION(!connection || connection->allocator);
    if (!connection) {
        return;
    }

    /* If the slot is not NULL, the connection is still connected, which should be prevented from calling this function
     */
    AWS_ASSERT(!connection->slot);

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: Destroying connection", (void *)connection);

    /* If the reconnect_task isn't freed, free it */
    if (connection->reconnect_task) {
        aws_mem_release(connection->reconnect_task->allocator, connection->reconnect_task);
    }
    aws_string_destroy(connection->host_name);

    /* Clear the credentials */
    if (connection->username) {
        aws_string_destroy_secure(connection->username);
    }
    if (connection->password) {
        aws_string_destroy_secure(connection->password);
    }

    /* Clean up the will */
    aws_byte_buf_clean_up(&connection->will.topic);
    aws_byte_buf_clean_up(&connection->will.payload);

    /* Clear the client_id */
    aws_byte_buf_clean_up(&connection->client_id);

    /* Free all of the active subscriptions */
    aws_mqtt_topic_tree_clean_up(&connection->thread_data.subscriptions);

    aws_hash_table_clean_up(&connection->synced_data.outstanding_requests_table);
    /* clean up the pending_requests if it's not empty */
    while (!aws_linked_list_empty(&connection->synced_data.pending_requests_list)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&connection->synced_data.pending_requests_list);
        struct aws_mqtt_request *request = AWS_CONTAINER_OF(node, struct aws_mqtt_request, list_node);
        /* Fire the callback and clean up the memory, as the connection get destroyed. */
        if (request->on_complete) {
            request->on_complete(
                connection, request->packet_id, AWS_ERROR_MQTT_CONNECTION_DESTROYED, request->on_complete_ud);
        }
        aws_memory_pool_release(&connection->synced_data.requests_pool, request);
    }
    aws_memory_pool_clean_up(&connection->synced_data.requests_pool);

    aws_mutex_clean_up(&connection->synced_data.lock);

    aws_tls_connection_options_clean_up(&connection->tls_options);

    /* Clean up the websocket proxy options */
    if (connection->http_proxy_config) {
        aws_http_proxy_config_destroy(connection->http_proxy_config);
        connection->http_proxy_config = NULL;
    }

    aws_mqtt_client_release(connection->client);

    /* Frees all allocated memory */
    aws_mem_release(connection->allocator, connection);
}

static void s_on_final_disconnect(struct aws_mqtt_client_connection *connection, void *userdata) {
    (void)userdata;

    s_mqtt_client_connection_destroy_final(connection);
}

static void s_mqtt_client_connection_start_destroy(struct aws_mqtt_client_connection *connection) {
    bool call_destroy_final = false;

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Last refcount on connection has been released, start destroying the connection.",
        (void *)connection);
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);
        if (connection->synced_data.state != AWS_MQTT_CLIENT_STATE_DISCONNECTED) {
            /*
             * We don't call the on_disconnect callback until we've transitioned to the DISCONNECTED state.  So it's
             * safe to change it now while we hold the lock since we know we're not DISCONNECTED yet.
             */
            connection->on_disconnect = s_on_final_disconnect;

            if (connection->synced_data.state != AWS_MQTT_CLIENT_STATE_DISCONNECTING) {
                mqtt_disconnect_impl(connection, AWS_ERROR_SUCCESS);
                AWS_LOGF_DEBUG(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: final refcount has been released, switch state to DISCONNECTING.",
                    (void *)connection);
                mqtt_connection_set_state(connection, AWS_MQTT_CLIENT_STATE_DISCONNECTING);
            }
        } else {
            call_destroy_final = true;
        }

        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (call_destroy_final) {
        s_mqtt_client_connection_destroy_final(connection);
    }
}

struct aws_mqtt_client_connection *aws_mqtt_client_connection_new(struct aws_mqtt_client *client) {
    AWS_PRECONDITION(client);

    struct aws_mqtt_client_connection *connection =
        aws_mem_calloc(client->allocator, 1, sizeof(struct aws_mqtt_client_connection));
    if (!connection) {
        return NULL;
    }

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: Creating new connection", (void *)connection);

    /* Initialize the client */
    connection->allocator = client->allocator;
    aws_ref_count_init(
        &connection->ref_count, connection, (aws_simple_completion_callback *)s_mqtt_client_connection_start_destroy);
    connection->client = aws_mqtt_client_acquire(client);
    AWS_ZERO_STRUCT(connection->synced_data);
    connection->synced_data.state = AWS_MQTT_CLIENT_STATE_DISCONNECTED;
    connection->reconnect_timeouts.min_sec = 1;
    connection->reconnect_timeouts.max_sec = 128;
    aws_linked_list_init(&connection->synced_data.pending_requests_list);
    aws_linked_list_init(&connection->thread_data.ongoing_requests_list);

    if (aws_mutex_init(&connection->synced_data.lock)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to initialize mutex, error %d (%s)",
            (void *)connection,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto failed_init_mutex;
    }

    if (aws_mqtt_topic_tree_init(&connection->thread_data.subscriptions, connection->allocator)) {

        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to initialize subscriptions topic_tree, error %d (%s)",
            (void *)connection,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto failed_init_subscriptions;
    }

    if (aws_memory_pool_init(
            &connection->synced_data.requests_pool, connection->allocator, 32, sizeof(struct aws_mqtt_request))) {

        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to initialize request pool, error %d (%s)",
            (void *)connection,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto failed_init_requests_pool;
    }

    if (aws_hash_table_init(
            &connection->synced_data.outstanding_requests_table,
            connection->allocator,
            sizeof(struct aws_mqtt_request *),
            s_hash_uint16_t,
            s_uint16_t_eq,
            NULL,
            NULL)) {

        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to initialize outstanding requests table, error %d (%s)",
            (void *)connection,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto failed_init_outstanding_requests_table;
    }

    /* Initialize the handler */
    connection->handler.alloc = connection->allocator;
    connection->handler.vtable = aws_mqtt_get_client_channel_vtable();
    connection->handler.impl = connection;

    return connection;

failed_init_outstanding_requests_table:
    aws_memory_pool_clean_up(&connection->synced_data.requests_pool);

failed_init_requests_pool:
    aws_mqtt_topic_tree_clean_up(&connection->thread_data.subscriptions);

failed_init_subscriptions:
    aws_mutex_clean_up(&connection->synced_data.lock);

failed_init_mutex:
    aws_mem_release(client->allocator, connection);

    return NULL;
}

struct aws_mqtt_client_connection *aws_mqtt_client_connection_acquire(struct aws_mqtt_client_connection *connection) {
    if (connection != NULL) {
        aws_ref_count_acquire(&connection->ref_count);
    }

    return connection;
}

void aws_mqtt_client_connection_release(struct aws_mqtt_client_connection *connection) {
    if (connection != NULL) {
        aws_ref_count_release(&connection->ref_count);
    }
}

/*******************************************************************************
 * Connection Configuration
 ******************************************************************************/

/* To configure the connection, ensure the state is DISCONNECTED or CONNECTED */
static int s_check_connection_state_for_configuration(struct aws_mqtt_client_connection *connection) {
    int result = AWS_OP_SUCCESS;
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);

        if (connection->synced_data.state != AWS_MQTT_CLIENT_STATE_DISCONNECTED &&
            connection->synced_data.state != AWS_MQTT_CLIENT_STATE_CONNECTED) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT_CLIENT,
                "id=%p: Connection is currently pending connect/disconnect. Unable to make configuration changes until "
                "pending operation completes.",
                (void *)connection);
            result = AWS_OP_ERR;
        }
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    return result;
}

int aws_mqtt_client_connection_set_will(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic,
    enum aws_mqtt_qos qos,
    bool retain,
    const struct aws_byte_cursor *payload) {

    AWS_PRECONDITION(connection);
    AWS_PRECONDITION(topic);
    if (s_check_connection_state_for_configuration(connection)) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    int result = AWS_OP_ERR;
    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Setting last will with topic \"" PRInSTR "\"",
        (void *)connection,
        AWS_BYTE_CURSOR_PRI(*topic));

    if (!aws_mqtt_is_valid_topic(topic)) {
        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Will topic is invalid", (void *)connection);
        return aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
    }

    struct aws_byte_buf local_topic_buf;
    struct aws_byte_buf local_payload_buf;
    AWS_ZERO_STRUCT(local_topic_buf);
    AWS_ZERO_STRUCT(local_payload_buf);
    struct aws_byte_buf topic_buf = aws_byte_buf_from_array(topic->ptr, topic->len);
    if (aws_byte_buf_init_copy(&local_topic_buf, connection->allocator, &topic_buf)) {
        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to copy will topic", (void *)connection);
        goto cleanup;
    }

    connection->will.qos = qos;
    connection->will.retain = retain;

    struct aws_byte_buf payload_buf = aws_byte_buf_from_array(payload->ptr, payload->len);
    if (aws_byte_buf_init_copy(&local_payload_buf, connection->allocator, &payload_buf)) {
        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to copy will body", (void *)connection);
        goto cleanup;
    }

    if (connection->will.topic.len) {
        AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Will has been set before, resetting it.", (void *)connection);
    }
    /* Succeed. */
    result = AWS_OP_SUCCESS;

    /* swap the local buffer with connection */
    struct aws_byte_buf temp = local_topic_buf;
    local_topic_buf = connection->will.topic;
    connection->will.topic = temp;
    temp = local_payload_buf;
    local_payload_buf = connection->will.payload;
    connection->will.payload = temp;

cleanup:
    aws_byte_buf_clean_up(&local_topic_buf);
    aws_byte_buf_clean_up(&local_payload_buf);

    return result;
}

int aws_mqtt_client_connection_set_login(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *username,
    const struct aws_byte_cursor *password) {

    AWS_PRECONDITION(connection);
    AWS_PRECONDITION(username);
    if (s_check_connection_state_for_configuration(connection)) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    int result = AWS_OP_ERR;
    AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Setting username and password", (void *)connection);

    struct aws_string *username_string = NULL;
    struct aws_string *password_string = NULL;

    username_string = aws_string_new_from_array(connection->allocator, username->ptr, username->len);
    if (!username_string) {
        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to copy username", (void *)connection);
        goto cleanup;
    }

    if (password) {
        password_string = aws_string_new_from_array(connection->allocator, password->ptr, password->len);
        if (!password_string) {
            AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to copy password", (void *)connection);
            goto cleanup;
        }
    }

    if (connection->username) {
        AWS_LOGF_TRACE(
            AWS_LS_MQTT_CLIENT, "id=%p: Login information has been set before, resetting it.", (void *)connection);
    }
    /* Succeed. */
    result = AWS_OP_SUCCESS;

    /* swap the local string with connection */
    struct aws_string *temp = username_string;
    username_string = connection->username;
    connection->username = temp;
    temp = password_string;
    password_string = connection->password;
    connection->password = temp;

cleanup:
    aws_string_destroy_secure(username_string);
    aws_string_destroy_secure(password_string);

    return result;
}

int aws_mqtt_client_connection_set_reconnect_timeout(
    struct aws_mqtt_client_connection *connection,
    uint64_t min_timeout,
    uint64_t max_timeout) {

    AWS_PRECONDITION(connection);
    if (s_check_connection_state_for_configuration(connection)) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Setting reconnect timeouts min: %" PRIu64 " max: %" PRIu64,
        (void *)connection,
        min_timeout,
        max_timeout);
    connection->reconnect_timeouts.min_sec = min_timeout;
    connection->reconnect_timeouts.max_sec = max_timeout;

    return AWS_OP_SUCCESS;
}

int aws_mqtt_client_connection_set_connection_interruption_handlers(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_connection_interrupted_fn *on_interrupted,
    void *on_interrupted_ud,
    aws_mqtt_client_on_connection_resumed_fn *on_resumed,
    void *on_resumed_ud) {

    AWS_PRECONDITION(connection);
    if (s_check_connection_state_for_configuration(connection)) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT, "id=%p: Setting connection interrupted and resumed handlers", (void *)connection);

    connection->on_interrupted = on_interrupted;
    connection->on_interrupted_ud = on_interrupted_ud;
    connection->on_resumed = on_resumed;
    connection->on_resumed_ud = on_resumed_ud;

    return AWS_OP_SUCCESS;
}

int aws_mqtt_client_connection_set_on_any_publish_handler(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_publish_received_fn *on_any_publish,
    void *on_any_publish_ud) {

    AWS_PRECONDITION(connection);
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);

        if (connection->synced_data.state == AWS_MQTT_CLIENT_STATE_CONNECTED) {
            mqtt_connection_unlock_synced_data(connection);
            AWS_LOGF_ERROR(
                AWS_LS_MQTT_CLIENT,
                "id=%p: Connection is connected, publishes may arrive anytime. Unable to set publish handler until "
                "offline.",
                (void *)connection);
            return aws_raise_error(AWS_ERROR_INVALID_STATE);
        }
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Setting on_any_publish handler", (void *)connection);

    connection->on_any_publish = on_any_publish;
    connection->on_any_publish_ud = on_any_publish_ud;

    return AWS_OP_SUCCESS;
}

/*******************************************************************************
 * Websockets
 ******************************************************************************/
#ifdef AWS_MQTT_WITH_WEBSOCKETS

int aws_mqtt_client_connection_use_websockets(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_transform_websocket_handshake_fn *transformer,
    void *transformer_ud,
    aws_mqtt_validate_websocket_handshake_fn *validator,
    void *validator_ud) {

    connection->websocket.handshake_transformer = transformer;
    connection->websocket.handshake_transformer_ud = transformer_ud;
    connection->websocket.handshake_validator = validator;
    connection->websocket.handshake_validator_ud = validator_ud;
    connection->websocket.enabled = true;

    AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Using websockets", (void *)connection);

    return AWS_OP_SUCCESS;
}

int aws_mqtt_client_connection_set_http_proxy_options(
    struct aws_mqtt_client_connection *connection,
    struct aws_http_proxy_options *proxy_options) {

    /* If there is existing proxy options, nuke em */
    if (connection->http_proxy_config) {
        aws_http_proxy_config_destroy(connection->http_proxy_config);
        connection->http_proxy_config = NULL;
    }

    connection->http_proxy_config =
        aws_http_proxy_config_new_tunneling_from_proxy_options(connection->allocator, proxy_options);

    return connection->http_proxy_config != NULL ? AWS_OP_SUCCESS : AWS_OP_ERR;
}

static void s_on_websocket_shutdown(struct aws_websocket *websocket, int error_code, void *user_data) {
    struct aws_mqtt_client_connection *connection = user_data;

    struct aws_channel *channel = connection->slot ? connection->slot->channel : NULL;

    s_mqtt_client_shutdown(connection->client->bootstrap, error_code, channel, connection);

    if (websocket) {
        aws_websocket_release(websocket);
    }
}

static void s_on_websocket_setup(
    struct aws_websocket *websocket,
    int error_code,
    int handshake_response_status,
    const struct aws_http_header *handshake_response_header_array,
    size_t num_handshake_response_headers,
    void *user_data) {

    (void)handshake_response_status;

    /* Setup callback contract is: if error_code is non-zero then websocket is NULL. */
    AWS_FATAL_ASSERT((error_code != 0) == (websocket == NULL));

    struct aws_mqtt_client_connection *connection = user_data;
    struct aws_channel *channel = NULL;

    if (connection->websocket.handshake_request) {
        aws_http_message_release(connection->websocket.handshake_request);
        connection->websocket.handshake_request = NULL;
    }

    if (websocket) {
        channel = aws_websocket_get_channel(websocket);
        AWS_ASSERT(channel);

        /* Websocket must be "converted" before the MQTT handler can be installed next to it. */
        if (aws_websocket_convert_to_midchannel_handler(websocket)) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT_CLIENT,
                "id=%p: Failed converting websocket, error %d (%s)",
                (void *)connection,
                aws_last_error(),
                aws_error_name(aws_last_error()));

            aws_channel_shutdown(channel, aws_last_error());
            return;
        }

        /* If validation callback is set, let the user accept/reject the handshake */
        if (connection->websocket.handshake_validator) {
            AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Validating websocket handshake response.", (void *)connection);

            if (connection->websocket.handshake_validator(
                    connection,
                    handshake_response_header_array,
                    num_handshake_response_headers,
                    connection->websocket.handshake_validator_ud)) {

                AWS_LOGF_ERROR(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: Failure reported by websocket handshake validator callback, error %d (%s)",
                    (void *)connection,
                    aws_last_error(),
                    aws_error_name(aws_last_error()));

                aws_channel_shutdown(channel, aws_last_error());
                return;
            }

            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT, "id=%p: Done validating websocket handshake response.", (void *)connection);
        }
    }

    /* Call into the channel-setup callback, the rest of the logic is the same. */
    s_mqtt_client_init(connection->client->bootstrap, error_code, channel, connection);
}

static aws_mqtt_transform_websocket_handshake_complete_fn s_websocket_handshake_transform_complete; /* fwd declare */

static int s_websocket_connect(struct aws_mqtt_client_connection *connection) {
    AWS_ASSERT(connection->websocket.enabled);

    /* These defaults were chosen because they're commmon in other MQTT libraries.
     * The user can modify the request in their transform callback if they need to. */
    const struct aws_byte_cursor default_path = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/mqtt");
    const struct aws_http_header default_protocol_header = {
        .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Sec-WebSocket-Protocol"),
        .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("mqtt"),
    };

    /* Build websocket handshake request */
    connection->websocket.handshake_request = aws_http_message_new_websocket_handshake_request(
        connection->allocator, default_path, aws_byte_cursor_from_string(connection->host_name));

    if (!connection->websocket.handshake_request) {
        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to generate websocket handshake request", (void *)connection);
        goto error;
    }

    if (aws_http_message_add_header(connection->websocket.handshake_request, default_protocol_header)) {
        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to generate websocket handshake request", (void *)connection);
        goto error;
    }

    /* If user registered a transform callback, call it and wait for transform_complete() to be called.
     * If no callback registered, call the transform_complete() function ourselves. */
    if (connection->websocket.handshake_transformer) {
        AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Transforming websocket handshake request.", (void *)connection);

        connection->websocket.handshake_transformer(
            connection->websocket.handshake_request,
            connection->websocket.handshake_transformer_ud,
            s_websocket_handshake_transform_complete,
            connection);

    } else {
        s_websocket_handshake_transform_complete(
            connection->websocket.handshake_request, AWS_ERROR_SUCCESS, connection);
    }

    return AWS_OP_SUCCESS;

error:
    aws_http_message_release(connection->websocket.handshake_request);
    connection->websocket.handshake_request = NULL;
    return AWS_OP_ERR;
}

static void s_websocket_handshake_transform_complete(
    struct aws_http_message *handshake_request,
    int error_code,
    void *complete_ctx) {

    struct aws_mqtt_client_connection *connection = complete_ctx;

    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failure reported by websocket handshake transform callback.",
            (void *)connection);

        goto error;
    }

    if (connection->websocket.handshake_transformer) {
        AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Done transforming websocket handshake request.", (void *)connection);
    }

    /* Call websocket connect() */
    struct aws_websocket_client_connection_options websocket_options = {
        .allocator = connection->allocator,
        .bootstrap = connection->client->bootstrap,
        .socket_options = &connection->socket_options,
        .tls_options = connection->tls_options.ctx ? &connection->tls_options : NULL,
        .host = aws_byte_cursor_from_string(connection->host_name),
        .port = connection->port,
        .handshake_request = handshake_request,
        .initial_window_size = 0, /* Prevent websocket data from arriving before the MQTT handler is installed */
        .user_data = connection,
        .on_connection_setup = s_on_websocket_setup,
        .on_connection_shutdown = s_on_websocket_shutdown,
    };

    struct aws_http_proxy_options proxy_options;
    AWS_ZERO_STRUCT(proxy_options);
    if (connection->http_proxy_config != NULL) {
        aws_http_proxy_options_init_from_config(&proxy_options, connection->http_proxy_config);
        websocket_options.proxy_options = &proxy_options;
    }

    if (aws_websocket_client_connect(&websocket_options)) {
        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to initiate websocket connection.", (void *)connection);
        error_code = aws_last_error();
        goto error;
    }

    /* Success */
    return;

error:
    /* Proceed to next step, telling it that we failed. */
    s_on_websocket_setup(NULL, error_code, -1, NULL, 0, connection);
}

#else  /* AWS_MQTT_WITH_WEBSOCKETS */
int aws_mqtt_client_connection_use_websockets(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_transform_websocket_handshake_fn *transformer,
    void *transformer_ud,
    aws_mqtt_validate_websocket_handshake_fn *validator,
    void *validator_ud) {

    (void)connection;
    (void)transformer;
    (void)transformer_ud;
    (void)validator;
    (void)validator_ud;

    AWS_LOGF_ERROR(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Cannot use websockets unless library is built with MQTT_WITH_WEBSOCKETS option.",
        (void *)connection);

    return aws_raise_error(AWS_ERROR_MQTT_BUILT_WITHOUT_WEBSOCKETS);
}

int aws_mqtt_client_connection_set_websocket_proxy_options(
    struct aws_mqtt_client_connection *connection,
    struct aws_http_proxy_options *proxy_options) {

    (void)connection;
    (void)proxy_options;

    AWS_LOGF_ERROR(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Cannot use websockets unless library is built with MQTT_WITH_WEBSOCKETS option.",
        (void *)connection);

    return aws_raise_error(AWS_ERROR_MQTT_BUILT_WITHOUT_WEBSOCKETS);
}
#endif /* AWS_MQTT_WITH_WEBSOCKETS */

/*******************************************************************************
 * Connect
 ******************************************************************************/

int aws_mqtt_client_connection_connect(
    struct aws_mqtt_client_connection *connection,
    const struct aws_mqtt_connection_options *connection_options) {

    /* TODO: Do we need to support resuming the connection if user connect to the same connection & endpoint and the
     * clean_session is false?
     * If not, the broker will resume the connection in this case, and we pretend we are making a new connection, which
     * may cause some confusing behavior. This is basically what we have now. NOTE: The topic_tree is living with the
     * connection right now, which is really confusing.
     * If yes, an edge case will be: User disconnected from the connection with clean_session
     * being false, then connect to another endpoint with the same connection object, we probably need to clear all
     * those states from last connection and create a new "connection". Problem is what if user finish the second
     * connection and reconnect to the first endpoint. There is no way for us to resume the connection in this case. */

    AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Opening connection", (void *)connection);
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);

        if (connection->synced_data.state != AWS_MQTT_CLIENT_STATE_DISCONNECTED) {
            mqtt_connection_unlock_synced_data(connection);
            return aws_raise_error(AWS_ERROR_MQTT_ALREADY_CONNECTED);
        }
        mqtt_connection_set_state(connection, AWS_MQTT_CLIENT_STATE_CONNECTING);
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_CLIENT, "id=%p: Begin connecting process, switch state to CONNECTING.", (void *)connection);
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (connection->host_name) {
        aws_string_destroy(connection->host_name);
    }

    connection->host_name = aws_string_new_from_array(
        connection->allocator, connection_options->host_name.ptr, connection_options->host_name.len);
    connection->port = connection_options->port;
    connection->socket_options = *connection_options->socket_options;
    connection->clean_session = connection_options->clean_session;
    connection->keep_alive_time_secs = connection_options->keep_alive_time_secs;
    connection->connection_count = 0;

    if (!connection->keep_alive_time_secs) {
        connection->keep_alive_time_secs = s_default_keep_alive_sec;
    }
    if (!connection_options->protocol_operation_timeout_ms) {
        connection->operation_timeout_ns = UINT64_MAX;
    } else {
        connection->operation_timeout_ns = aws_timestamp_convert(
            (uint64_t)connection_options->protocol_operation_timeout_ms,
            AWS_TIMESTAMP_MILLIS,
            AWS_TIMESTAMP_NANOS,
            NULL);
    }

    if (!connection_options->ping_timeout_ms) {
        connection->ping_timeout_ns = s_default_ping_timeout_ns;
    } else {
        connection->ping_timeout_ns = aws_timestamp_convert(
            (uint64_t)connection_options->ping_timeout_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);
    }

    /* Keep alive time should always be greater than the timeouts. */
    if (AWS_UNLIKELY(connection->keep_alive_time_secs * (uint64_t)AWS_TIMESTAMP_NANOS <= connection->ping_timeout_ns)) {
        AWS_LOGF_FATAL(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Illegal configuration, Connection keep alive %" PRIu64
            "ns must be greater than the request timeouts %" PRIu64 "ns.",
            (void *)connection,
            (uint64_t)connection->keep_alive_time_secs * (uint64_t)AWS_TIMESTAMP_NANOS,
            connection->ping_timeout_ns);
        AWS_FATAL_ASSERT(
            connection->keep_alive_time_secs * (uint64_t)AWS_TIMESTAMP_NANOS > connection->ping_timeout_ns);
    }

    AWS_LOGF_INFO(
        AWS_LS_MQTT_CLIENT,
        "id=%p: using ping timeout of %" PRIu64 " ns",
        (void *)connection,
        connection->ping_timeout_ns);

    /* Cheat and set the tls_options host_name to our copy if they're the same */
    if (connection_options->tls_options) {
        connection->use_tls = true;
        if (aws_tls_connection_options_copy(&connection->tls_options, connection_options->tls_options)) {

            AWS_LOGF_ERROR(
                AWS_LS_MQTT_CLIENT, "id=%p: Failed to copy TLS Connection Options into connection", (void *)connection);
            return AWS_OP_ERR;
        }

        if (!connection_options->tls_options->server_name) {
            struct aws_byte_cursor host_name_cur = aws_byte_cursor_from_string(connection->host_name);
            if (aws_tls_connection_options_set_server_name(
                    &connection->tls_options, connection->allocator, &host_name_cur)) {

                AWS_LOGF_ERROR(
                    AWS_LS_MQTT_CLIENT, "id=%p: Failed to set TLS Connection Options server name", (void *)connection);
                goto error;
            }
        }

    } else {
        AWS_ZERO_STRUCT(connection->tls_options);
    }

    /* Clean up old client_id */
    if (connection->client_id.buffer) {
        aws_byte_buf_clean_up(&connection->client_id);
    }

    /* Only set connection->client_id if a new one was provided */
    struct aws_byte_buf client_id_buf =
        aws_byte_buf_from_array(connection_options->client_id.ptr, connection_options->client_id.len);
    if (aws_byte_buf_init_copy(&connection->client_id, connection->allocator, &client_id_buf)) {
        AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Failed to copy client_id into connection", (void *)connection);
        goto error;
    }

    struct aws_linked_list cancelling_requests;
    aws_linked_list_init(&cancelling_requests);
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);
        if (connection->clean_session) {
            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT,
                "id=%p: a clean session connection requested, all the previous requests will fail",
                (void *)connection);
            aws_linked_list_swap_contents(&connection->synced_data.pending_requests_list, &cancelling_requests);
        }
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (!aws_linked_list_empty(&cancelling_requests)) {

        struct aws_linked_list_node *current = aws_linked_list_front(&cancelling_requests);
        const struct aws_linked_list_node *end = aws_linked_list_end(&cancelling_requests);
        /* invoke all the complete callback for requests from previous session */
        while (current != end) {
            struct aws_mqtt_request *request = AWS_CONTAINER_OF(current, struct aws_mqtt_request, list_node);
            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT,
                "id=%p: Establishing a new clean session connection, discard the previous request %" PRIu16,
                (void *)connection,
                request->packet_id);
            if (request->on_complete) {
                request->on_complete(
                    connection,
                    request->packet_id,
                    AWS_ERROR_MQTT_CANCELLED_FOR_CLEAN_SESSION,
                    request->on_complete_ud);
            }
            current = current->next;
        }
        /* free the resource */
        { /* BEGIN CRITICAL SECTION */
            mqtt_connection_lock_synced_data(connection);
            while (!aws_linked_list_empty(&cancelling_requests)) {
                struct aws_linked_list_node *node = aws_linked_list_pop_front(&cancelling_requests);
                struct aws_mqtt_request *request = AWS_CONTAINER_OF(node, struct aws_mqtt_request, list_node);
                aws_hash_table_remove(
                    &connection->synced_data.outstanding_requests_table, &request->packet_id, NULL, NULL);
                aws_memory_pool_release(&connection->synced_data.requests_pool, request);
            }
            mqtt_connection_unlock_synced_data(connection);
        } /* END CRITICAL SECTION */
    }

    /* Begin the connecting process, acquire the connection to keep it alive until we disconnected */
    aws_mqtt_client_connection_acquire(connection);

    if (s_mqtt_client_connect(connection, connection_options->on_connection_complete, connection_options->user_data)) {
        /*
         * An error calling s_mqtt_client_connect should (must) be mutually exclusive with s_mqtt_client_shutdown().
         * So it should be safe and correct to call release now to undo the pinning we did a few lines above.
         */
        aws_mqtt_client_connection_release(connection);

        /* client_id has been updated with something but it will get cleaned up when the connection gets cleaned up
         * so we don't need to worry about it here*/
        if (connection->clean_session) {
            AWS_LOGF_WARN(
                AWS_LS_MQTT_CLIENT, "id=%p: The previous session has been cleaned up and losted!", (void *)connection);
        }
        goto error;
    }

    return AWS_OP_SUCCESS;

error:
    aws_tls_connection_options_clean_up(&connection->tls_options);
    AWS_ZERO_STRUCT(connection->tls_options);
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);
        mqtt_connection_set_state(connection, AWS_MQTT_CLIENT_STATE_DISCONNECTED);
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    return AWS_OP_ERR;
}

static int s_mqtt_client_connect(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_connection_complete_fn *on_connection_complete,
    void *userdata) {
    connection->on_connection_complete = on_connection_complete;
    connection->on_connection_complete_ud = userdata;

    int result = 0;
#ifdef AWS_MQTT_WITH_WEBSOCKETS
    if (connection->websocket.enabled) {
        result = s_websocket_connect(connection);
    } else
#endif /* AWS_MQTT_WITH_WEBSOCKETS */
    {
        struct aws_socket_channel_bootstrap_options channel_options;
        AWS_ZERO_STRUCT(channel_options);
        channel_options.bootstrap = connection->client->bootstrap;
        channel_options.host_name = aws_string_c_str(connection->host_name);
        channel_options.port = connection->port;
        channel_options.socket_options = &connection->socket_options;
        channel_options.tls_options = connection->use_tls ? &connection->tls_options : NULL;
        channel_options.setup_callback = &s_mqtt_client_init;
        channel_options.shutdown_callback = &s_mqtt_client_shutdown;
        channel_options.user_data = connection;

        if (connection->http_proxy_config == NULL) {
            result = aws_client_bootstrap_new_socket_channel(&channel_options);
        } else {
            struct aws_http_proxy_options proxy_options;
            AWS_ZERO_STRUCT(proxy_options);

            aws_http_proxy_options_init_from_config(&proxy_options, connection->http_proxy_config);
            result = aws_http_proxy_new_socket_channel(&channel_options, &proxy_options);
        }
    }

    if (result) {
        /* Connection attempt failed */
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to begin connection routine, error %d (%s).",
            (void *)connection,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/*******************************************************************************
 * Reconnect  DEPRECATED
 ******************************************************************************/

int aws_mqtt_client_connection_reconnect(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_connection_complete_fn *on_connection_complete,
    void *userdata) {
    (void)connection;
    (void)on_connection_complete;
    (void)userdata;

    /* DEPRECATED, connection will reconnect automatically now. */
    AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "aws_mqtt_client_connection_reconnect has been DEPRECATED.");
    return aws_raise_error(AWS_ERROR_UNSUPPORTED_OPERATION);
}

/*******************************************************************************
 * Disconnect
 ******************************************************************************/

int aws_mqtt_client_connection_disconnect(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_disconnect_fn *on_disconnect,
    void *userdata) {

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: user called disconnect.", (void *)connection);

    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);

        if (connection->synced_data.state != AWS_MQTT_CLIENT_STATE_CONNECTED &&
            connection->synced_data.state != AWS_MQTT_CLIENT_STATE_RECONNECTING) {
            mqtt_connection_unlock_synced_data(connection);
            AWS_LOGF_ERROR(
                AWS_LS_MQTT_CLIENT, "id=%p: Connection is not open, and may not be closed", (void *)connection);
            aws_raise_error(AWS_ERROR_MQTT_NOT_CONNECTED);
            return AWS_OP_ERR;
        }
        mqtt_connection_set_state(connection, AWS_MQTT_CLIENT_STATE_DISCONNECTING);
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_CLIENT,
            "id=%p: User requests disconnecting, switch state to DISCONNECTING.",
            (void *)connection);
        connection->on_disconnect = on_disconnect;
        connection->on_disconnect_ud = userdata;
        connection->reconnect_timeouts.next_attempt_reset_timer_ns = 0;
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: Closing connection", (void *)connection);

    mqtt_disconnect_impl(connection, AWS_OP_SUCCESS);

    return AWS_OP_SUCCESS;
}

/*******************************************************************************
 * Subscribe
 ******************************************************************************/

static void s_on_publish_client_wrapper(
    const struct aws_byte_cursor *topic,
    const struct aws_byte_cursor *payload,
    bool dup,
    enum aws_mqtt_qos qos,
    bool retain,
    void *userdata) {

    struct subscribe_task_topic *task_topic = userdata;

    /* Call out to the user callback */
    if (task_topic->request.on_publish) {
        task_topic->request.on_publish(
            task_topic->connection, topic, payload, dup, qos, retain, task_topic->request.on_publish_ud);
    }
}

static void s_task_topic_release(void *userdata) {
    struct subscribe_task_topic *task_topic = userdata;
    if (task_topic != NULL) {
        aws_ref_count_release(&task_topic->ref_count);
    }
}

static void s_task_topic_clean_up(void *userdata) {

    struct subscribe_task_topic *task_topic = userdata;

    if (task_topic->request.on_cleanup) {
        task_topic->request.on_cleanup(task_topic->request.on_publish_ud);
    }
    aws_string_destroy(task_topic->filter);
    aws_mem_release(task_topic->connection->allocator, task_topic);
}

static enum aws_mqtt_client_request_state s_subscribe_send(uint16_t packet_id, bool is_first_attempt, void *userdata) {

    (void)is_first_attempt;

    struct subscribe_task_arg *task_arg = userdata;
    bool initing_packet = task_arg->subscribe.fixed_header.packet_type == 0;
    struct aws_io_message *message = NULL;

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Attempting send of subscribe %" PRIu16 " (%s)",
        (void *)task_arg->connection,
        packet_id,
        is_first_attempt ? "first attempt" : "resend");

    if (initing_packet) {
        /* Init the subscribe packet */
        if (aws_mqtt_packet_subscribe_init(&task_arg->subscribe, task_arg->connection->allocator, packet_id)) {
            return AWS_MQTT_CLIENT_REQUEST_ERROR;
        }
    }

    const size_t num_topics = aws_array_list_length(&task_arg->topics);
    if (num_topics <= 0) {
        aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
        return AWS_MQTT_CLIENT_REQUEST_ERROR;
    }

    AWS_VARIABLE_LENGTH_ARRAY(uint8_t, transaction_buf, num_topics * aws_mqtt_topic_tree_action_size);
    struct aws_array_list transaction;
    aws_array_list_init_static(&transaction, transaction_buf, num_topics, aws_mqtt_topic_tree_action_size);

    for (size_t i = 0; i < num_topics; ++i) {

        struct subscribe_task_topic *topic = NULL;
        aws_array_list_get_at(&task_arg->topics, &topic, i);
        AWS_ASSUME(topic); /* We know we're within bounds */

        if (initing_packet) {
            if (aws_mqtt_packet_subscribe_add_topic(&task_arg->subscribe, topic->request.topic, topic->request.qos)) {
                goto handle_error;
            }
        }

        if (!task_arg->tree_updated) {
            if (aws_mqtt_topic_tree_transaction_insert(
                    &task_arg->connection->thread_data.subscriptions,
                    &transaction,
                    topic->filter,
                    topic->request.qos,
                    s_on_publish_client_wrapper,
                    s_task_topic_release,
                    topic)) {

                goto handle_error;
            }
            /* If insert succeed, acquire the refcount */
            aws_ref_count_acquire(&topic->ref_count);
        }
    }

    message = mqtt_get_message_for_packet(task_arg->connection, &task_arg->subscribe.fixed_header);
    if (!message) {

        goto handle_error;
    }

    if (aws_mqtt_packet_subscribe_encode(&message->message_data, &task_arg->subscribe)) {

        goto handle_error;
    }

    /* This is not necessarily a fatal error; if the subscribe fails, it'll just retry. Still need to clean up though.
     */
    if (aws_channel_slot_send_message(task_arg->connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
        aws_mem_release(message->allocator, message);
    }

    if (!task_arg->tree_updated) {
        aws_mqtt_topic_tree_transaction_commit(&task_arg->connection->thread_data.subscriptions, &transaction);
        task_arg->tree_updated = true;
    }

    aws_array_list_clean_up(&transaction);
    return AWS_MQTT_CLIENT_REQUEST_ONGOING;

handle_error:

    if (message) {
        aws_mem_release(message->allocator, message);
    }
    if (!task_arg->tree_updated) {
        aws_mqtt_topic_tree_transaction_roll_back(&task_arg->connection->thread_data.subscriptions, &transaction);
    }

    aws_array_list_clean_up(&transaction);
    return AWS_MQTT_CLIENT_REQUEST_ERROR;
}

static void s_subscribe_complete(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    int error_code,
    void *userdata) {

    struct subscribe_task_arg *task_arg = userdata;

    struct subscribe_task_topic *topic = NULL;
    aws_array_list_get_at(&task_arg->topics, &topic, 0);
    AWS_ASSUME(topic);

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Subscribe %" PRIu16 " completed with error_code %d",
        (void *)connection,
        packet_id,
        error_code);

    size_t list_len = aws_array_list_length(&task_arg->topics);
    if (task_arg->on_suback.multi) {
        /* create a list of aws_mqtt_topic_subscription pointers from topics for the callback */
        AWS_VARIABLE_LENGTH_ARRAY(uint8_t, cb_list_buf, list_len * sizeof(void *));
        struct aws_array_list cb_list;
        aws_array_list_init_static(&cb_list, cb_list_buf, list_len, sizeof(void *));
        int err = 0;
        for (size_t i = 0; i < list_len; i++) {
            err |= aws_array_list_get_at(&task_arg->topics, &topic, i);
            struct aws_mqtt_topic_subscription *subscription = &topic->request;
            err |= aws_array_list_push_back(&cb_list, &subscription);
        }
        AWS_ASSUME(!err);
        task_arg->on_suback.multi(connection, packet_id, &cb_list, error_code, task_arg->on_suback_ud);
        aws_array_list_clean_up(&cb_list);
    } else if (task_arg->on_suback.single) {
        task_arg->on_suback.single(
            connection, packet_id, &topic->request.topic, topic->request.qos, error_code, task_arg->on_suback_ud);
    }
    for (size_t i = 0; i < list_len; i++) {
        aws_array_list_get_at(&task_arg->topics, &topic, i);
        s_task_topic_release(topic);
    }
    aws_array_list_clean_up(&task_arg->topics);
    aws_mqtt_packet_subscribe_clean_up(&task_arg->subscribe);
    aws_mem_release(task_arg->connection->allocator, task_arg);
}

uint16_t aws_mqtt_client_connection_subscribe_multiple(
    struct aws_mqtt_client_connection *connection,
    const struct aws_array_list *topic_filters,
    aws_mqtt_suback_multi_fn *on_suback,
    void *on_suback_ud) {

    AWS_PRECONDITION(connection);

    struct subscribe_task_arg *task_arg = aws_mem_calloc(connection->allocator, 1, sizeof(struct subscribe_task_arg));
    if (!task_arg) {
        return 0;
    }

    task_arg->connection = connection;
    task_arg->on_suback.multi = on_suback;
    task_arg->on_suback_ud = on_suback_ud;

    const size_t num_topics = aws_array_list_length(topic_filters);

    if (aws_array_list_init_dynamic(&task_arg->topics, connection->allocator, num_topics, sizeof(void *))) {
        goto handle_error;
    }

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: Starting multi-topic subscribe", (void *)connection);

    for (size_t i = 0; i < num_topics; ++i) {

        struct aws_mqtt_topic_subscription *request = NULL;
        aws_array_list_get_at_ptr(topic_filters, (void **)&request, i);

        if (!aws_mqtt_is_valid_topic_filter(&request->topic)) {
            aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
            goto handle_error;
        }

        struct subscribe_task_topic *task_topic =
            aws_mem_calloc(connection->allocator, 1, sizeof(struct subscribe_task_topic));
        if (!task_topic) {
            goto handle_error;
        }
        aws_ref_count_init(&task_topic->ref_count, task_topic, (aws_simple_completion_callback *)s_task_topic_clean_up);

        task_topic->connection = connection;
        task_topic->request = *request;

        task_topic->filter = aws_string_new_from_array(
            connection->allocator, task_topic->request.topic.ptr, task_topic->request.topic.len);
        if (!task_topic->filter) {
            aws_mem_release(connection->allocator, task_topic);
            goto handle_error;
        }

        /* Update request topic cursor to refer to owned string */
        task_topic->request.topic = aws_byte_cursor_from_string(task_topic->filter);

        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_CLIENT,
            "id=%p:     Adding topic \"" PRInSTR "\"",
            (void *)connection,
            AWS_BYTE_CURSOR_PRI(task_topic->request.topic));

        /* Push into the list */
        aws_array_list_push_back(&task_arg->topics, &task_topic);
    }

    uint16_t packet_id = mqtt_create_request(
        task_arg->connection, &s_subscribe_send, task_arg, &s_subscribe_complete, task_arg, false /* noRetry */);

    if (packet_id == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to kick off multi-topic subscribe, with error %s",
            (void *)connection,
            aws_error_debug_str(aws_last_error()));
        goto handle_error;
    }

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: Sending multi-topic subscribe %" PRIu16, (void *)connection, packet_id);
    return packet_id;

handle_error:

    if (task_arg) {

        if (task_arg->topics.data) {

            const size_t num_added_topics = aws_array_list_length(&task_arg->topics);
            for (size_t i = 0; i < num_added_topics; ++i) {

                struct subscribe_task_topic *task_topic = NULL;
                aws_array_list_get_at(&task_arg->topics, (void **)&task_topic, i);
                AWS_ASSUME(task_topic);

                aws_string_destroy(task_topic->filter);
                aws_mem_release(connection->allocator, task_topic);
            }

            aws_array_list_clean_up(&task_arg->topics);
        }

        aws_mem_release(connection->allocator, task_arg);
    }
    return 0;
}

/*******************************************************************************
 * Subscribe Single
 ******************************************************************************/

static void s_subscribe_single_complete(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    int error_code,
    void *userdata) {

    struct subscribe_task_arg *task_arg = userdata;

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Subscribe %" PRIu16 " completed with error code %d",
        (void *)connection,
        packet_id,
        error_code);

    AWS_ASSERT(aws_array_list_length(&task_arg->topics) == 1);
    struct subscribe_task_topic *topic = NULL;
    aws_array_list_get_at(&task_arg->topics, &topic, 0);
    AWS_ASSUME(topic); /* There needs to be exactly 1 topic in this list */
    if (task_arg->on_suback.single) {
        AWS_ASSUME(aws_string_is_valid(topic->filter));
        aws_mqtt_suback_fn *suback = task_arg->on_suback.single;
        suback(connection, packet_id, &topic->request.topic, topic->request.qos, error_code, task_arg->on_suback_ud);
    }
    s_task_topic_release(topic);
    aws_array_list_clean_up(&task_arg->topics);
    aws_mqtt_packet_subscribe_clean_up(&task_arg->subscribe);
    aws_mem_release(task_arg->connection->allocator, task_arg);
}

uint16_t aws_mqtt_client_connection_subscribe(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic_filter,
    enum aws_mqtt_qos qos,
    aws_mqtt_client_publish_received_fn *on_publish,
    void *on_publish_ud,
    aws_mqtt_userdata_cleanup_fn *on_ud_cleanup,
    aws_mqtt_suback_fn *on_suback,
    void *on_suback_ud) {

    AWS_PRECONDITION(connection);

    if (!aws_mqtt_is_valid_topic_filter(topic_filter)) {
        aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
        return 0;
    }

    /* Because we know we're only going to have 1 topic, we can cheat and allocate the array_list in the same block as
     * the task argument. */
    void *task_topic_storage = NULL;
    struct subscribe_task_topic *task_topic = NULL;
    struct subscribe_task_arg *task_arg = aws_mem_acquire_many(
        connection->allocator,
        2,
        &task_arg,
        sizeof(struct subscribe_task_arg),
        &task_topic_storage,
        sizeof(struct subscribe_task_topic *));

    if (!task_arg) {
        goto handle_error;
    }
    AWS_ZERO_STRUCT(*task_arg);

    task_arg->connection = connection;
    task_arg->on_suback.single = on_suback;
    task_arg->on_suback_ud = on_suback_ud;

    /* It stores the pointer */
    aws_array_list_init_static(&task_arg->topics, task_topic_storage, 1, sizeof(void *));

    /* Allocate the topic and push into the list */
    task_topic = aws_mem_calloc(connection->allocator, 1, sizeof(struct subscribe_task_topic));
    if (!task_topic) {
        goto handle_error;
    }
    aws_ref_count_init(&task_topic->ref_count, task_topic, (aws_simple_completion_callback *)s_task_topic_clean_up);
    aws_array_list_push_back(&task_arg->topics, &task_topic);

    task_topic->filter = aws_string_new_from_array(connection->allocator, topic_filter->ptr, topic_filter->len);
    if (!task_topic->filter) {
        goto handle_error;
    }

    task_topic->connection = connection;
    task_topic->request.topic = aws_byte_cursor_from_string(task_topic->filter);
    task_topic->request.qos = qos;
    task_topic->request.on_publish = on_publish;
    task_topic->request.on_cleanup = on_ud_cleanup;
    task_topic->request.on_publish_ud = on_publish_ud;

    uint16_t packet_id = mqtt_create_request(
        task_arg->connection, &s_subscribe_send, task_arg, &s_subscribe_single_complete, task_arg, false /* noRetry */);

    if (packet_id == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to start subscribe on topic " PRInSTR " with error %s",
            (void *)connection,
            AWS_BYTE_CURSOR_PRI(task_topic->request.topic),
            aws_error_debug_str(aws_last_error()));
        goto handle_error;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Starting subscribe %" PRIu16 " on topic " PRInSTR,
        (void *)connection,
        packet_id,
        AWS_BYTE_CURSOR_PRI(task_topic->request.topic));

    return packet_id;

handle_error:

    if (task_topic) {
        if (task_topic->filter) {
            aws_string_destroy(task_topic->filter);
        }
        aws_mem_release(connection->allocator, task_topic);
    }

    if (task_arg) {
        aws_mem_release(connection->allocator, task_arg);
    }

    return 0;
}

/*******************************************************************************
 * Subscribe Local
 ******************************************************************************/

/* The lifetime of this struct is from subscribe -> suback */
struct subscribe_local_task_arg {

    struct aws_mqtt_client_connection *connection;

    struct subscribe_task_topic *task_topic;

    aws_mqtt_suback_fn *on_suback;
    void *on_suback_ud;
};

static enum aws_mqtt_client_request_state s_subscribe_local_send(
    uint16_t packet_id,
    bool is_first_attempt,
    void *userdata) {

    (void)is_first_attempt;

    struct subscribe_local_task_arg *task_arg = userdata;

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Attempting save of local subscribe %" PRIu16 " (%s)",
        (void *)task_arg->connection,
        packet_id,
        is_first_attempt ? "first attempt" : "redo");

    struct subscribe_task_topic *topic = task_arg->task_topic;
    if (aws_mqtt_topic_tree_insert(
            &task_arg->connection->thread_data.subscriptions,
            topic->filter,
            topic->request.qos,
            s_on_publish_client_wrapper,
            s_task_topic_release,
            topic)) {

        return AWS_MQTT_CLIENT_REQUEST_ERROR;
    }
    aws_ref_count_acquire(&topic->ref_count);

    return AWS_MQTT_CLIENT_REQUEST_COMPLETE;
}

static void s_subscribe_local_complete(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    int error_code,
    void *userdata) {

    struct subscribe_local_task_arg *task_arg = userdata;

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Local subscribe %" PRIu16 " completed with error code %d",
        (void *)connection,
        packet_id,
        error_code);

    struct subscribe_task_topic *topic = task_arg->task_topic;
    if (task_arg->on_suback) {
        aws_mqtt_suback_fn *suback = task_arg->on_suback;
        suback(connection, packet_id, &topic->request.topic, topic->request.qos, error_code, task_arg->on_suback_ud);
    }
    s_task_topic_release(topic);

    aws_mem_release(task_arg->connection->allocator, task_arg);
}

uint16_t aws_mqtt_client_connection_subscribe_local(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic_filter,
    aws_mqtt_client_publish_received_fn *on_publish,
    void *on_publish_ud,
    aws_mqtt_userdata_cleanup_fn *on_ud_cleanup,
    aws_mqtt_suback_fn *on_suback,
    void *on_suback_ud) {

    AWS_PRECONDITION(connection);

    if (!aws_mqtt_is_valid_topic_filter(topic_filter)) {
        aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
        return 0;
    }

    struct subscribe_task_topic *task_topic = NULL;

    struct subscribe_local_task_arg *task_arg =
        aws_mem_calloc(connection->allocator, 1, sizeof(struct subscribe_local_task_arg));

    if (!task_arg) {
        goto handle_error;
    }
    AWS_ZERO_STRUCT(*task_arg);

    task_arg->connection = connection;
    task_arg->on_suback = on_suback;
    task_arg->on_suback_ud = on_suback_ud;
    task_topic = aws_mem_calloc(connection->allocator, 1, sizeof(struct subscribe_task_topic));
    if (!task_topic) {
        goto handle_error;
    }
    aws_ref_count_init(&task_topic->ref_count, task_topic, (aws_simple_completion_callback *)s_task_topic_clean_up);
    task_arg->task_topic = task_topic;

    task_topic->filter = aws_string_new_from_array(connection->allocator, topic_filter->ptr, topic_filter->len);
    if (!task_topic->filter) {
        goto handle_error;
    }

    task_topic->connection = connection;
    task_topic->is_local = true;
    task_topic->request.topic = aws_byte_cursor_from_string(task_topic->filter);
    task_topic->request.on_publish = on_publish;
    task_topic->request.on_cleanup = on_ud_cleanup;
    task_topic->request.on_publish_ud = on_publish_ud;

    uint16_t packet_id = mqtt_create_request(
        task_arg->connection,
        s_subscribe_local_send,
        task_arg,
        &s_subscribe_local_complete,
        task_arg,
        false /* noRetry */);

    if (packet_id == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to start local subscribe on topic " PRInSTR " with error %s",
            (void *)connection,
            AWS_BYTE_CURSOR_PRI(task_topic->request.topic),
            aws_error_debug_str(aws_last_error()));
        goto handle_error;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Starting local subscribe %" PRIu16 " on topic " PRInSTR,
        (void *)connection,
        packet_id,
        AWS_BYTE_CURSOR_PRI(task_topic->request.topic));
    return packet_id;

handle_error:

    if (task_topic) {
        if (task_topic->filter) {
            aws_string_destroy(task_topic->filter);
        }
        aws_mem_release(connection->allocator, task_topic);
    }

    if (task_arg) {
        aws_mem_release(connection->allocator, task_arg);
    }

    return 0;
}

/*******************************************************************************
 * Resubscribe
 ******************************************************************************/

static bool s_reconnect_resub_iterator(const struct aws_byte_cursor *topic, enum aws_mqtt_qos qos, void *user_data) {
    struct subscribe_task_arg *task_arg = user_data;

    struct subscribe_task_topic *task_topic =
        aws_mem_calloc(task_arg->connection->allocator, 1, sizeof(struct subscribe_task_topic));
    struct aws_mqtt_topic_subscription sub;
    AWS_ZERO_STRUCT(sub);
    sub.topic = *topic;
    sub.qos = qos;
    task_topic->request = sub;
    task_topic->connection = task_arg->connection;

    aws_array_list_push_back(&task_arg->topics, &task_topic);
    aws_ref_count_init(&task_topic->ref_count, task_topic, (aws_simple_completion_callback *)s_task_topic_clean_up);
    return true;
}

static enum aws_mqtt_client_request_state s_resubscribe_send(
    uint16_t packet_id,
    bool is_first_attempt,
    void *userdata) {

    struct subscribe_task_arg *task_arg = userdata;
    bool initing_packet = task_arg->subscribe.fixed_header.packet_type == 0;
    struct aws_io_message *message = NULL;

    const size_t sub_count = aws_mqtt_topic_tree_get_sub_count(&task_arg->connection->thread_data.subscriptions);
    /* Init the topics list even if there are no topics because the s_resubscribe_complete callback will always run. */
    if (aws_array_list_init_dynamic(&task_arg->topics, task_arg->connection->allocator, sub_count, sizeof(void *))) {
        goto handle_error;
    }
    if (sub_count == 0) {
        AWS_LOGF_TRACE(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Not subscribed to any topics. Resubscribe is unnecessary, no packet will be sent.",
            (void *)task_arg->connection);
        return AWS_MQTT_CLIENT_REQUEST_COMPLETE;
    }
    aws_mqtt_topic_tree_iterate(&task_arg->connection->thread_data.subscriptions, s_reconnect_resub_iterator, task_arg);

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Attempting send of resubscribe %" PRIu16 " (%s)",
        (void *)task_arg->connection,
        packet_id,
        is_first_attempt ? "first attempt" : "resend");

    if (initing_packet) {
        /* Init the subscribe packet */
        if (aws_mqtt_packet_subscribe_init(&task_arg->subscribe, task_arg->connection->allocator, packet_id)) {
            return AWS_MQTT_CLIENT_REQUEST_ERROR;
        }

        const size_t num_topics = aws_array_list_length(&task_arg->topics);
        if (num_topics <= 0) {
            aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
            return AWS_MQTT_CLIENT_REQUEST_ERROR;
        }

        for (size_t i = 0; i < num_topics; ++i) {

            struct subscribe_task_topic *topic = NULL;
            aws_array_list_get_at(&task_arg->topics, &topic, i);
            AWS_ASSUME(topic); /* We know we're within bounds */

            if (aws_mqtt_packet_subscribe_add_topic(&task_arg->subscribe, topic->request.topic, topic->request.qos)) {
                goto handle_error;
            }
        }
    }

    message = mqtt_get_message_for_packet(task_arg->connection, &task_arg->subscribe.fixed_header);
    if (!message) {

        goto handle_error;
    }

    if (aws_mqtt_packet_subscribe_encode(&message->message_data, &task_arg->subscribe)) {

        goto handle_error;
    }

    /* This is not necessarily a fatal error; if the send fails, it'll just retry.  Still need to clean up though. */
    if (aws_channel_slot_send_message(task_arg->connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
        aws_mem_release(message->allocator, message);
    }

    return AWS_MQTT_CLIENT_REQUEST_ONGOING;

handle_error:

    if (message) {
        aws_mem_release(message->allocator, message);
    }

    return AWS_MQTT_CLIENT_REQUEST_ERROR;
}

static void s_resubscribe_complete(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    int error_code,
    void *userdata) {

    struct subscribe_task_arg *task_arg = userdata;

    const size_t list_len = aws_array_list_length(&task_arg->topics);
    if (list_len <= 0) {
        goto clean_up;
    }

    struct subscribe_task_topic *topic = NULL;
    aws_array_list_get_at(&task_arg->topics, &topic, 0);
    AWS_ASSUME(topic);

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Subscribe %" PRIu16 " completed with error_code %d",
        (void *)connection,
        packet_id,
        error_code);

    if (task_arg->on_suback.multi) {
        /* create a list of aws_mqtt_topic_subscription pointers from topics for the callback */
        AWS_VARIABLE_LENGTH_ARRAY(uint8_t, cb_list_buf, list_len * sizeof(void *));
        struct aws_array_list cb_list;
        aws_array_list_init_static(&cb_list, cb_list_buf, list_len, sizeof(void *));
        int err = 0;
        for (size_t i = 0; i < list_len; i++) {
            err |= aws_array_list_get_at(&task_arg->topics, &topic, i);
            struct aws_mqtt_topic_subscription *subscription = &topic->request;
            err |= aws_array_list_push_back(&cb_list, &subscription);
        }
        AWS_ASSUME(!err);
        task_arg->on_suback.multi(connection, packet_id, &cb_list, error_code, task_arg->on_suback_ud);
        aws_array_list_clean_up(&cb_list);
    } else if (task_arg->on_suback.single) {
        task_arg->on_suback.single(
            connection, packet_id, &topic->request.topic, topic->request.qos, error_code, task_arg->on_suback_ud);
    }

clean_up:

    /* We need to cleanup the subscribe_task_topics, since they are not inserted into the topic tree by resubscribe. We
     * take the ownership to clean it up */
    for (size_t i = 0; i < list_len; i++) {
        aws_array_list_get_at(&task_arg->topics, &topic, i);
        s_task_topic_release(topic);
    }
    aws_array_list_clean_up(&task_arg->topics);
    aws_mqtt_packet_subscribe_clean_up(&task_arg->subscribe);
    aws_mem_release(task_arg->connection->allocator, task_arg);
}

uint16_t aws_mqtt_resubscribe_existing_topics(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_suback_multi_fn *on_suback,
    void *on_suback_ud) {

    struct subscribe_task_arg *task_arg = aws_mem_calloc(connection->allocator, 1, sizeof(struct subscribe_task_arg));
    if (!task_arg) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT, "id=%p: failed to allocate storage for resubscribe arguments", (void *)connection);
        return 0;
    }

    AWS_ZERO_STRUCT(*task_arg);
    task_arg->connection = connection;
    task_arg->on_suback.multi = on_suback;
    task_arg->on_suback_ud = on_suback_ud;

    uint16_t packet_id = mqtt_create_request(
        task_arg->connection, &s_resubscribe_send, task_arg, &s_resubscribe_complete, task_arg, false /* noRetry */);

    if (packet_id == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to send multi-topic resubscribe with error %s",
            (void *)connection,
            aws_error_name(aws_last_error()));
        goto handle_error;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT, "id=%p: Sending multi-topic resubscribe %" PRIu16, (void *)connection, packet_id);

    return packet_id;

handle_error:

    aws_mem_release(connection->allocator, task_arg);

    return 0;
}

/*******************************************************************************
 * Unsubscribe
 ******************************************************************************/

struct unsubscribe_task_arg {
    struct aws_mqtt_client_connection *connection;
    struct aws_string *filter_string;
    struct aws_byte_cursor filter;
    bool is_local;
    /* Packet to populate */
    struct aws_mqtt_packet_unsubscribe unsubscribe;

    /* true if transaction was committed to the topic tree, false requires a retry */
    bool tree_updated;

    aws_mqtt_op_complete_fn *on_unsuback;
    void *on_unsuback_ud;

    struct request_timeout_wrapper timeout_wrapper;
};

static enum aws_mqtt_client_request_state s_unsubscribe_send(
    uint16_t packet_id,
    bool is_first_attempt,
    void *userdata) {

    (void)is_first_attempt;

    struct unsubscribe_task_arg *task_arg = userdata;
    struct aws_io_message *message = NULL;

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Attempting send of unsubscribe %" PRIu16 " %s",
        (void *)task_arg->connection,
        packet_id,
        is_first_attempt ? "first attempt" : "resend");

    static const size_t num_topics = 1;

    AWS_VARIABLE_LENGTH_ARRAY(uint8_t, transaction_buf, num_topics * aws_mqtt_topic_tree_action_size);
    struct aws_array_list transaction;
    aws_array_list_init_static(&transaction, transaction_buf, num_topics, aws_mqtt_topic_tree_action_size);

    if (!task_arg->tree_updated) {

        struct subscribe_task_topic *topic;
        if (aws_mqtt_topic_tree_transaction_remove(
                &task_arg->connection->thread_data.subscriptions, &transaction, &task_arg->filter, (void **)&topic)) {
            goto handle_error;
        }

        task_arg->is_local = topic ? topic->is_local : false;
    }

    if (!task_arg->is_local) {
        if (task_arg->unsubscribe.fixed_header.packet_type == 0) {
            /* If unsubscribe packet is uninitialized, init it */
            if (aws_mqtt_packet_unsubscribe_init(&task_arg->unsubscribe, task_arg->connection->allocator, packet_id)) {
                goto handle_error;
            }
            if (aws_mqtt_packet_unsubscribe_add_topic(&task_arg->unsubscribe, task_arg->filter)) {
                goto handle_error;
            }
        }

        message = mqtt_get_message_for_packet(task_arg->connection, &task_arg->unsubscribe.fixed_header);
        if (!message) {
            goto handle_error;
        }

        if (aws_mqtt_packet_unsubscribe_encode(&message->message_data, &task_arg->unsubscribe)) {
            goto handle_error;
        }

        if (aws_channel_slot_send_message(task_arg->connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
            goto handle_error;
        }

        /* TODO: timing should start from the message written into the socket, which is aws_io_message->on_completion
         * invoked, but there are bugs in the websocket handler (and maybe also the h1 handler?) where we don't properly
         * fire the on_completion callbacks. */
        struct request_timeout_task_arg *timeout_task_arg = s_schedule_timeout_task(task_arg->connection, packet_id);
        if (!timeout_task_arg) {
            return AWS_MQTT_CLIENT_REQUEST_ERROR;
        }

        /*
         * Set up mutual references between the operation task args and the timeout task args.  Whoever runs first
         * "wins", does its logic, and then breaks the connection between the two.
         */
        task_arg->timeout_wrapper.timeout_task_arg = timeout_task_arg;
        timeout_task_arg->task_arg_wrapper = &task_arg->timeout_wrapper;
    }

    if (!task_arg->tree_updated) {
        aws_mqtt_topic_tree_transaction_commit(&task_arg->connection->thread_data.subscriptions, &transaction);
        task_arg->tree_updated = true;
    }

    aws_array_list_clean_up(&transaction);
    /* If the subscribe is local-only, don't wait for a SUBACK to come back. */
    return task_arg->is_local ? AWS_MQTT_CLIENT_REQUEST_COMPLETE : AWS_MQTT_CLIENT_REQUEST_ONGOING;

handle_error:

    if (message) {
        aws_mem_release(message->allocator, message);
    }
    if (!task_arg->tree_updated) {
        aws_mqtt_topic_tree_transaction_roll_back(&task_arg->connection->thread_data.subscriptions, &transaction);
    }

    aws_array_list_clean_up(&transaction);
    return AWS_MQTT_CLIENT_REQUEST_ERROR;
}

static void s_unsubscribe_complete(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    int error_code,
    void *userdata) {

    struct unsubscribe_task_arg *task_arg = userdata;

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: Unsubscribe %" PRIu16 " complete", (void *)connection, packet_id);

    /*
     * If we have a forward pointer to a timeout task, then that means the timeout task has not run yet.  So we should
     * follow it and zero out the back pointer to us, because we're going away now.  The timeout task will run later
     * and be harmless (even vs. future operations with the same packet id) because it only cancels if it has a back
     * pointer.
     */
    if (task_arg->timeout_wrapper.timeout_task_arg) {
        task_arg->timeout_wrapper.timeout_task_arg->task_arg_wrapper = NULL;
        task_arg->timeout_wrapper.timeout_task_arg = NULL;
    }

    if (task_arg->on_unsuback) {
        task_arg->on_unsuback(connection, packet_id, error_code, task_arg->on_unsuback_ud);
    }

    aws_string_destroy(task_arg->filter_string);
    aws_mqtt_packet_unsubscribe_clean_up(&task_arg->unsubscribe);
    aws_mem_release(task_arg->connection->allocator, task_arg);
}

uint16_t aws_mqtt_client_connection_unsubscribe(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic_filter,
    aws_mqtt_op_complete_fn *on_unsuback,
    void *on_unsuback_ud) {

    AWS_PRECONDITION(connection);

    if (!aws_mqtt_is_valid_topic_filter(topic_filter)) {
        aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
        return 0;
    }

    struct unsubscribe_task_arg *task_arg =
        aws_mem_calloc(connection->allocator, 1, sizeof(struct unsubscribe_task_arg));
    if (!task_arg) {
        return 0;
    }

    task_arg->connection = connection;
    task_arg->filter_string = aws_string_new_from_array(connection->allocator, topic_filter->ptr, topic_filter->len);
    task_arg->filter = aws_byte_cursor_from_string(task_arg->filter_string);
    task_arg->on_unsuback = on_unsuback;
    task_arg->on_unsuback_ud = on_unsuback_ud;

    uint16_t packet_id = mqtt_create_request(
        connection, &s_unsubscribe_send, task_arg, s_unsubscribe_complete, task_arg, false /* noRetry */);
    if (packet_id == 0) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed to start unsubscribe, with error %s",
            (void *)connection,
            aws_error_debug_str(aws_last_error()));
        goto handle_error;
    }

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: Starting unsubscribe %" PRIu16, (void *)connection, packet_id);

    return packet_id;

handle_error:

    aws_string_destroy(task_arg->filter_string);
    aws_mem_release(connection->allocator, task_arg);

    return 0;
}

/*******************************************************************************
 * Publish
 ******************************************************************************/

struct publish_task_arg {
    struct aws_mqtt_client_connection *connection;
    struct aws_string *topic_string;
    struct aws_byte_cursor topic;
    enum aws_mqtt_qos qos;
    bool retain;
    struct aws_byte_cursor payload;
    struct aws_byte_buf payload_buf;

    /* Packet to populate */
    struct aws_mqtt_packet_publish publish;

    aws_mqtt_op_complete_fn *on_complete;
    void *userdata;

    struct request_timeout_wrapper timeout_wrapper;
};

/* should only be called by tests */
static int s_get_stuff_from_outstanding_requests_table(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    struct aws_allocator *allocator,
    struct aws_byte_buf *result_buf,
    struct aws_string **result_string) {

    int err = AWS_OP_SUCCESS;

    aws_mutex_lock(&connection->synced_data.lock);
    struct aws_hash_element *elem = NULL;
    aws_hash_table_find(&connection->synced_data.outstanding_requests_table, &packet_id, &elem);
    if (elem) {
        struct aws_mqtt_request *request = elem->value;
        struct publish_task_arg *pub = (struct publish_task_arg *)request->send_request_ud;
        if (result_buf != NULL) {
            if (aws_byte_buf_init_copy(result_buf, allocator, &pub->payload_buf)) {
                err = AWS_OP_ERR;
            }
        } else if (result_string != NULL) {
            *result_string = aws_string_new_from_string(allocator, pub->topic_string);
            if (*result_string == NULL) {
                err = AWS_OP_ERR;
            }
        }
    } else {
        /* So lovely that this error is defined, but hashtable never actually raises it */
        err = aws_raise_error(AWS_ERROR_HASHTBL_ITEM_NOT_FOUND);
    }
    aws_mutex_unlock(&connection->synced_data.lock);

    return err;
}

/* should only be called by tests */
int aws_mqtt_client_get_payload_for_outstanding_publish_packet(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    struct aws_allocator *allocator,
    struct aws_byte_buf *result) {

    AWS_ZERO_STRUCT(*result);
    return s_get_stuff_from_outstanding_requests_table(connection, packet_id, allocator, result, NULL);
}

/* should only be called by tests */
int aws_mqtt_client_get_topic_for_outstanding_publish_packet(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    struct aws_allocator *allocator,
    struct aws_string **result) {

    *result = NULL;
    return s_get_stuff_from_outstanding_requests_table(connection, packet_id, allocator, NULL, result);
}

static enum aws_mqtt_client_request_state s_publish_send(uint16_t packet_id, bool is_first_attempt, void *userdata) {
    struct publish_task_arg *task_arg = userdata;
    struct aws_mqtt_client_connection *connection = task_arg->connection;

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Attempting send of publish %" PRIu16 " %s",
        (void *)task_arg->connection,
        packet_id,
        is_first_attempt ? "first attempt" : "resend");

    bool is_qos_0 = task_arg->qos == AWS_MQTT_QOS_AT_MOST_ONCE;
    if (is_qos_0) {
        packet_id = 0;
    }

    if (is_first_attempt) {
        if (aws_mqtt_packet_publish_init(
                &task_arg->publish,
                task_arg->retain,
                task_arg->qos,
                !is_first_attempt,
                task_arg->topic,
                packet_id,
                task_arg->payload)) {

            return AWS_MQTT_CLIENT_REQUEST_ERROR;
        }
    }

    struct aws_io_message *message = mqtt_get_message_for_packet(task_arg->connection, &task_arg->publish.fixed_header);
    if (!message) {
        return AWS_MQTT_CLIENT_REQUEST_ERROR;
    }

    /* Encode the headers, and everything but the payload */
    if (aws_mqtt_packet_publish_encode_headers(&message->message_data, &task_arg->publish)) {
        return AWS_MQTT_CLIENT_REQUEST_ERROR;
    }

    struct aws_byte_cursor payload_cur = task_arg->payload;
    {
    write_payload_chunk:
        (void)NULL;

        const size_t left_in_message = message->message_data.capacity - message->message_data.len;
        const size_t to_write = payload_cur.len < left_in_message ? payload_cur.len : left_in_message;

        if (to_write) {
            /* Write this chunk */
            struct aws_byte_cursor to_write_cur = aws_byte_cursor_advance(&payload_cur, to_write);
            AWS_ASSERT(to_write_cur.ptr); /* to_write is guaranteed to be inside the bounds of payload_cur */
            if (!aws_byte_buf_write_from_whole_cursor(&message->message_data, to_write_cur)) {

                aws_mem_release(message->allocator, message);
                return AWS_MQTT_CLIENT_REQUEST_ERROR;
            }
        }

        if (aws_channel_slot_send_message(task_arg->connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
            aws_mem_release(message->allocator, message);
            /* If it's QoS 0, telling user that the message haven't been sent, else, the message will be resent once the
             * connection is back */
            return is_qos_0 ? AWS_MQTT_CLIENT_REQUEST_ERROR : AWS_MQTT_CLIENT_REQUEST_ONGOING;
        }

        /* If there's still payload left, get a new message and start again. */
        if (payload_cur.len) {
            message = mqtt_get_message_for_packet(task_arg->connection, &task_arg->publish.fixed_header);
            goto write_payload_chunk;
        }
    }
    if (!is_qos_0 && connection->operation_timeout_ns != UINT64_MAX) {
        /* TODO: timing should start from the message written into the socket, which is aws_io_message->on_completion
         * invoked, but there are bugs in the websocket handler (and maybe also the h1 handler?) where we don't properly
         * fire fire the on_completion callbacks. */
        struct request_timeout_task_arg *timeout_task_arg = s_schedule_timeout_task(connection, packet_id);
        if (!timeout_task_arg) {
            return AWS_MQTT_CLIENT_REQUEST_ERROR;
        }

        /*
         * Set up mutual references between the operation task args and the timeout task args.  Whoever runs first
         * "wins", does its logic, and then breaks the connection between the two.
         */
        task_arg->timeout_wrapper.timeout_task_arg = timeout_task_arg;
        timeout_task_arg->task_arg_wrapper = &task_arg->timeout_wrapper;
    }

    /* If QoS == 0, there will be no ack, so consider the request done now. */
    return is_qos_0 ? AWS_MQTT_CLIENT_REQUEST_COMPLETE : AWS_MQTT_CLIENT_REQUEST_ONGOING;
}

static void s_publish_complete(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    int error_code,
    void *userdata) {
    struct publish_task_arg *task_arg = userdata;

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: Publish %" PRIu16 " complete", (void *)connection, packet_id);

    if (task_arg->on_complete) {
        task_arg->on_complete(connection, packet_id, error_code, task_arg->userdata);
    }

    /*
     * If we have a forward pointer to a timeout task, then that means the timeout task has not run yet.  So we should
     * follow it and zero out the back pointer to us, because we're going away now.  The timeout task will run later
     * and be harmless (even vs. future operations with the same packet id) because it only cancels if it has a back
     * pointer.
     */
    if (task_arg->timeout_wrapper.timeout_task_arg != NULL) {
        task_arg->timeout_wrapper.timeout_task_arg->task_arg_wrapper = NULL;
        task_arg->timeout_wrapper.timeout_task_arg = NULL;
    }

    aws_byte_buf_clean_up(&task_arg->payload_buf);
    aws_string_destroy(task_arg->topic_string);
    aws_mem_release(connection->allocator, task_arg);
}

uint16_t aws_mqtt_client_connection_publish(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic,
    enum aws_mqtt_qos qos,
    bool retain,
    const struct aws_byte_cursor *payload,
    aws_mqtt_op_complete_fn *on_complete,
    void *userdata) {

    AWS_PRECONDITION(connection);

    if (!aws_mqtt_is_valid_topic(topic)) {
        aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
        return 0;
    }

    struct publish_task_arg *arg = aws_mem_calloc(connection->allocator, 1, sizeof(struct publish_task_arg));
    if (!arg) {
        return 0;
    }

    arg->connection = connection;
    arg->topic_string = aws_string_new_from_array(connection->allocator, topic->ptr, topic->len);
    arg->topic = aws_byte_cursor_from_string(arg->topic_string);
    arg->qos = qos;
    arg->retain = retain;
    if (aws_byte_buf_init_copy_from_cursor(&arg->payload_buf, connection->allocator, *payload)) {
        goto handle_error;
    }
    arg->payload = aws_byte_cursor_from_buf(&arg->payload_buf);
    arg->on_complete = on_complete;
    arg->userdata = userdata;

    bool retry = qos == AWS_MQTT_QOS_AT_MOST_ONCE;
    uint16_t packet_id = mqtt_create_request(connection, &s_publish_send, arg, &s_publish_complete, arg, retry);

    if (packet_id == 0) {
        /* bummer, we failed to make a new request */
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Failed starting publish to topic " PRInSTR ",error %d (%s)",
            (void *)connection,
            AWS_BYTE_CURSOR_PRI(*topic),
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto handle_error;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Starting publish %" PRIu16 " to topic " PRInSTR,
        (void *)connection,
        packet_id,
        AWS_BYTE_CURSOR_PRI(*topic));
    return packet_id;

handle_error:

    /* we know arg is valid, topic_string may or may not be valid */
    if (arg->topic_string) {
        aws_string_destroy(arg->topic_string);
    }

    aws_byte_buf_clean_up(&arg->payload_buf);

    aws_mem_release(connection->allocator, arg);

    return 0;
}

/*******************************************************************************
 * Ping
 ******************************************************************************/

static void s_pingresp_received_timeout(struct aws_channel_task *channel_task, void *arg, enum aws_task_status status) {
    struct aws_mqtt_client_connection *connection = arg;

    if (status == AWS_TASK_STATUS_RUN_READY) {
        /* Check that a pingresp has been received since pingreq was sent */
        if (connection->thread_data.waiting_on_ping_response) {
            connection->thread_data.waiting_on_ping_response = false;
            /* It's been too long since the last ping, close the connection */
            AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: ping timeout detected", (void *)connection);
            aws_channel_shutdown(connection->slot->channel, AWS_ERROR_MQTT_TIMEOUT);
        }
    }

    aws_mem_release(connection->allocator, channel_task);
}

static enum aws_mqtt_client_request_state s_pingreq_send(uint16_t packet_id, bool is_first_attempt, void *userdata) {
    (void)packet_id;
    (void)is_first_attempt;
    AWS_PRECONDITION(is_first_attempt);

    struct aws_mqtt_client_connection *connection = userdata;

    AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: pingreq send", (void *)connection);
    struct aws_mqtt_packet_connection pingreq;
    aws_mqtt_packet_pingreq_init(&pingreq);

    struct aws_io_message *message = mqtt_get_message_for_packet(connection, &pingreq.fixed_header);
    if (!message) {
        return AWS_MQTT_CLIENT_REQUEST_ERROR;
    }

    if (aws_mqtt_packet_connection_encode(&message->message_data, &pingreq)) {
        aws_mem_release(message->allocator, message);
        return AWS_MQTT_CLIENT_REQUEST_ERROR;
    }

    if (aws_channel_slot_send_message(connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
        aws_mem_release(message->allocator, message);
        return AWS_MQTT_CLIENT_REQUEST_ERROR;
    }

    /* Mark down that now is when the last pingreq was sent */
    connection->thread_data.waiting_on_ping_response = true;

    struct aws_channel_task *ping_timeout_task =
        aws_mem_calloc(connection->allocator, 1, sizeof(struct aws_channel_task));
    if (!ping_timeout_task) {
        /* allocation failed, no log, just return error. */
        goto error;
    }
    aws_channel_task_init(ping_timeout_task, s_pingresp_received_timeout, connection, "mqtt_pingresp_timeout");
    uint64_t now = 0;
    if (aws_channel_current_clock_time(connection->slot->channel, &now)) {
        goto error;
    }
    now += connection->ping_timeout_ns;
    aws_channel_schedule_task_future(connection->slot->channel, ping_timeout_task, now);
    return AWS_MQTT_CLIENT_REQUEST_COMPLETE;

error:
    return AWS_MQTT_CLIENT_REQUEST_ERROR;
}

int aws_mqtt_client_connection_ping(struct aws_mqtt_client_connection *connection) {

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: Starting ping", (void *)connection);

    uint16_t packet_id = mqtt_create_request(connection, &s_pingreq_send, connection, NULL, NULL, true /* noRetry */);

    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: Starting ping with packet id %" PRIu16, (void *)connection, packet_id);

    return (packet_id > 0) ? AWS_OP_SUCCESS : AWS_OP_ERR;
}
