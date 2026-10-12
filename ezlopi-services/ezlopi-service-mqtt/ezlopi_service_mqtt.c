/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**
** 1. Redistributions of source code must retain the above copyright notice,
**    this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. Neither the name of the copyright holder nor the names of its
**    contributors may be used to endorse or promote products derived from
**    this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
** POSSIBILITY OF SUCH DAMAGE.
** ===========================================================================
*/
/**
 * @file    ezlopi_service_mqtt.c
 * @brief   Publish this hub's items to an MQTT broker for Home Assistant.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#ifdef CONFIG_EZPI_ENABLE_MQTT

#include "esp_mac.h"
#include "mqtt_client.h"

#include "cjext.h"
#include "ezlopi_util_ha.h"
#include "ezlopi_util_trace.h"

#include "ezlopi_core_broadcast.h"
#include "ezlopi_core_cjson_macros.h"
#include "ezlopi_core_devices.h"
#include "ezlopi_core_errors.h"
#include "ezlopi_core_factory_info.h"

#include "ezlopi_cloud_constants.h"
#include "ezlopi_cloud_items.h"

#include "ezlopi_service_mqtt.h"

#include "EZLOPI_USER_CONFIG.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/* Long enough for the longest topic this file builds: a discovery prefix, a
 * platform name, the node id and an object id whose device-name half is capped
 * well below this. The topic builders refuse to truncate, so an overflow shows
 * up as nothing being published rather than as a wrong topic. */
#define EZPI_MQTT_TOPIC_LEN 192

/* The object id is the reduced device name plus 17 bytes of ids. 64 leaves
 * room for a generous name; longer names lose their tail, which is cosmetic. */
#define EZPI_MQTT_OBJECT_ID_LEN 64

/* A state payload is one scalar: a bool, a number, or a short string. Strings
 * longer than this are truncated rather than dropped -- a clipped reading is
 * more useful on a dashboard than a missing one, and no string item in the
 * tree is anywhere near this long. */
#define EZPI_MQTT_PAYLOAD_LEN 160

#define EZPI_MQTT_ONLINE_STR "online"
#define EZPI_MQTT_OFFLINE_STR "offline"

/* The last segment of each topic. Spelled out here rather than borrowed from
 * the ezlopi_*_str constants in ezlopi-cloud, even where one happens to hold
 * the same word: those name fields in the Ezlo API, and if one were ever
 * renamed every topic here would move with it, which would orphan every
 * entity a user has already built automations against.
 *
 * EZPI_ha_parse_command_topic() also hard-codes the "/set" it accepts, so
 * changing this macro alone would break inbound commands. */
#define EZPI_MQTT_LEAF_STATE "state"
#define EZPI_MQTT_LEAF_SET "set"
#define EZPI_MQTT_LEAF_STATUS "status"

/* Published as both the discovery payload_on/payload_off and accepted on the
 * command topic. They match what cJSON prints for a bool, which is what the
 * item-updated broadcast carries, so the state topic and the discovery
 * declaration cannot drift apart. */
#define EZPI_MQTT_TRUE_STR "true"
#define EZPI_MQTT_FALSE_STR "false"

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static void ezpi_mqtt_build_node_id(void);
static ezlopi_error_t ezpi_mqtt_on_broadcast(char *data);
static void ezpi_mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data);
static void ezpi_mqtt_announce(void);
static void ezpi_mqtt_publish_discovery_for_item(l_ezlopi_device_t *device,
                                                 l_ezlopi_item_t *item);
static void ezpi_mqtt_handle_command(const char *topic, int topic_len,
                                     const char *data, int data_len);
static bool ezpi_mqtt_value_to_payload(const cJSON *cj_value, const char *value_type,
                                       char *dst, size_t dst_size);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static esp_mqtt_client_handle_t sg_client = NULL;

/* "ezlopi_" plus six hex digits of MAC, so 14 bytes and a NUL. */
static char sg_node_id[24] = {0};

/* Written on the MQTT client's task, read on the loop-service task. Only ever
 * used to skip work that would be pointless while disconnected, so a stale
 * read costs one enqueue that esp-mqtt then drops. */
static volatile bool sg_connected = false;

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
void EZPI_service_mqtt_init(void)
{
    char availability_topic[EZPI_MQTT_TOPIC_LEN];
    esp_mqtt_client_config_t mqtt_cfg;

    if (NULL != sg_client)
    {
        TRACE_W("mqtt: already initialised");
        return;
    }

    /* An unset broker URI is a configuration that cannot work, as distinct
     * from a broker that is merely unreachable right now. Retrying the former
     * forever would fill the log with failures that look like a network
     * problem. */
    if ('\0' == CONFIG_EZPI_MQTT_BROKER_URI[0])
    {
        TRACE_W("mqtt: no broker URI configured, not starting");
        return;
    }

    ezpi_mqtt_build_node_id();

    if (0 == EZPI_ha_topic_build(availability_topic, sizeof(availability_topic),
                                 CONFIG_EZPI_MQTT_TOPIC_PREFIX, sg_node_id, NULL,
                                 EZPI_MQTT_LEAF_STATUS))
    {
        TRACE_E("mqtt: availability topic does not fit, not starting");
        return;
    }

    memset(&mqtt_cfg, 0, sizeof(mqtt_cfg));
    mqtt_cfg.uri = CONFIG_EZPI_MQTT_BROKER_URI;
    mqtt_cfg.client_id = sg_node_id;

    if ('\0' != CONFIG_EZPI_MQTT_USERNAME[0])
    {
        mqtt_cfg.username = CONFIG_EZPI_MQTT_USERNAME;
        mqtt_cfg.password = CONFIG_EZPI_MQTT_PASSWORD;
    }

    /* The last will is what makes a hub that loses power show as unavailable
     * in Home Assistant rather than freezing on its final readings. Every
     * entity's availability_topic points here, so one retained "offline" from
     * the broker greys out the whole hub. Without this the dashboard keeps
     * showing a temperature from a device that has been unplugged for a week.
     *
     * esp-mqtt copies the config strings, so the stack buffer is safe. */
    mqtt_cfg.lwt_topic = availability_topic;
    mqtt_cfg.lwt_msg = EZPI_MQTT_OFFLINE_STR;
    mqtt_cfg.lwt_msg_len = (int)strlen(EZPI_MQTT_OFFLINE_STR);
    mqtt_cfg.lwt_retain = 1;
    mqtt_cfg.lwt_qos = 1;

    /* Deliberately not keepalive-tuned or gated on connectivity. See the
     * header: waiting on the Wi-Fi event bit would hang on an Ethernet-only
     * board, and esp-mqtt's own retry does not care which interface appears. */
    sg_client = esp_mqtt_client_init(&mqtt_cfg);
    if (NULL == sg_client)
    {
        TRACE_E("mqtt: client init failed");
        return;
    }

    if (ESP_OK != esp_mqtt_client_register_event(sg_client, ESP_EVENT_ANY_ID,
                                                 ezpi_mqtt_event_handler, NULL))
    {
        TRACE_E("mqtt: event registration failed");
        esp_mqtt_client_destroy(sg_client);
        sg_client = NULL;
        return;
    }

    /* Observing the existing broadcast rather than adding a hook to the core.
     * The retry count is 0 because __call_broadcast_methods breaks out of its
     * retry loop on both branches, so the value has no effect -- passing
     * something larger would only suggest otherwise. */
    if (NULL == EZPI_core_broadcast_method_add(ezpi_mqtt_on_broadcast, "ha-mqtt", 0))
    {
        TRACE_E("mqtt: could not register broadcast observer, state will not publish");
    }

    if (ESP_OK != esp_mqtt_client_start(sg_client))
    {
        TRACE_E("mqtt: client start failed");
        return;
    }

    TRACE_I("mqtt: started as '%s' -> %s", sg_node_id, CONFIG_EZPI_MQTT_BROKER_URI);
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * @brief Derive this hub's node id from the MAC address.
 *
 * Not from EZPI_core_factory_info_v3_get_id(): that returns 0 until the hub
 * has been provisioned against the Ezlo cloud, and a board flashed from this
 * repository and pointed at a local broker never is. The MAC is readable from
 * efuse on first boot, is stable across reflashes, and is unique per board,
 * which is everything a node id needs to be.
 */
static void ezpi_mqtt_build_node_id(void)
{
    uint8_t mac[6] = {0};

    if (ESP_OK != esp_read_mac(mac, ESP_MAC_WIFI_STA))
    {
        /* Without a MAC two hubs on one broker would collide. Keeping a fixed
         * id is still better than refusing to start: one hub is the common
         * case, and the symptom of a collision is visible, whereas a hub that
         * silently never appears is not. */
        TRACE_E("mqtt: could not read MAC, using a fixed node id");
        snprintf(sg_node_id, sizeof(sg_node_id), "ezlopi_000000");
        return;
    }

    snprintf(sg_node_id, sizeof(sg_node_id), "ezlopi_%02x%02x%02x",
             mac[3], mac[4], mac[5]);
}

/**
 * @brief Turn one item's value into the scalar a state topic carries.
 *
 * Scalars rather than a JSON object with a value_template: the template would
 * have to be declared in the discovery payload and kept in step with what is
 * published here, and a mismatch between the two is exactly the kind of fault
 * that produces an entity stuck at "unknown" with nothing logged anywhere.
 *
 * @return false when the value cannot be represented, in which case nothing
 *         should be published -- an empty payload on a state topic is read by
 *         Home Assistant as a value, not as an absence.
 */
static bool ezpi_mqtt_value_to_payload(const cJSON *cj_value, const char *value_type,
                                       char *dst, size_t dst_size)
{
    (void)value_type;

    if (NULL == cj_value || NULL == dst || 0 == dst_size)
    {
        return false;
    }

    dst[0] = '\0';

    switch (cj_value->type & 0xFF)
    {
    case cJSON_True:
    {
        return (size_t)snprintf(dst, dst_size, EZPI_MQTT_TRUE_STR) < dst_size;
    }
    case cJSON_False:
    {
        return (size_t)snprintf(dst, dst_size, EZPI_MQTT_FALSE_STR) < dst_size;
    }
    case cJSON_Number:
    {
        /* %g rather than %f: a humidity of 54 should read "54", not
         * "54.000000", and %g gives six significant digits, which is more
         * than any sensor in the tree resolves to. */
        return (size_t)snprintf(dst, dst_size, "%g", cj_value->valuedouble) < dst_size;
    }
    case cJSON_String:
    {
        if (NULL == cj_value->valuestring)
        {
            return false;
        }

        /* Truncation is accepted here, unlike for topics: a clipped string
         * reading is still a reading, where a clipped topic goes nowhere. */
        snprintf(dst, dst_size, "%s", cj_value->valuestring);
        return true;
    }
    default:
    {
        break;
    }
    }

    /* Objects, arrays and null reach here. EZPI_ha_describe() refuses the
     * value types that produce them, so an item of this shape has no entity
     * and nothing is listening to its state topic anyway. */
    return false;
}

/**
 * @brief Observe one item-update broadcast and publish the new state.
 *
 * Runs on the loop-service task with the global serialization buffer held, so
 * it parses, enqueues and returns. Nothing here waits on the network.
 *
 * Always returns success. The only caller discards the value, but
 * __call_broadcast_methods overwrites a shared result with each method's
 * return, and reporting a failure that the Ezlo path would then carry is not
 * this observer's business: an MQTT broker being down says nothing about
 * whether the cloud broadcast worked.
 */
static ezlopi_error_t ezpi_mqtt_on_broadcast(char *data)
{
    cJSON *cj_root = NULL;
    cJSON *cj_result = NULL;
    cJSON *cj_subclass = NULL;
    cJSON *cj_value = NULL;
    cJSON *cj_device_name = NULL;
    uint32_t device_id = 0;
    uint32_t item_id = 0;
    char object_id[EZPI_MQTT_OBJECT_ID_LEN];
    char topic[EZPI_MQTT_TOPIC_LEN];
    char payload[EZPI_MQTT_PAYLOAD_LEN];

    if (NULL == data || NULL == sg_client || false == sg_connected)
    {
        return EZPI_SUCCESS;
    }

    /* Cheap reject before committing to a parse. Most broadcasts are item
     * updates, but mode changes, settings and RPC replies all come through
     * here too and parsing those only to discard them would spend the loop
     * task's time for nothing. */
    if (NULL == strstr(data, method_hub_item_updated))
    {
        return EZPI_SUCCESS;
    }

    cj_root = cJSON_Parse(__FUNCTION__, data);
    if (NULL == cj_root)
    {
        return EZPI_SUCCESS;
    }

    cj_subclass = cJSON_GetObjectItem(__FUNCTION__, cj_root, ezlopi_msg_subclass_str);
    cj_result = cJSON_GetObjectItem(__FUNCTION__, cj_root, ezlopi_result_str);

    /* strstr matched somewhere in the document; this is the check that it
     * matched the field that decides what the message is. */
    if (NULL == cj_result || NULL == cj_subclass || cJSON_String != cj_subclass->type ||
        NULL == cj_subclass->valuestring ||
        0 != strcmp(cj_subclass->valuestring, method_hub_item_updated))
    {
        cJSON_Delete(__FUNCTION__, cj_root);
        return EZPI_SUCCESS;
    }

    device_id = EZPI_core_cjson_get_id(cj_result, ezlopi_deviceId_str);
    item_id = EZPI_core_cjson_get_id(cj_result, ezlopi__id_str);
    cj_value = cJSON_GetObjectItem(__FUNCTION__, cj_result, ezlopi_value_str);
    cj_device_name = cJSON_GetObjectItem(__FUNCTION__, cj_result, ezlopi_deviceName_str);

    if (0 == device_id || 0 == item_id)
    {
        cJSON_Delete(__FUNCTION__, cj_root);
        return EZPI_SUCCESS;
    }

    /* The device name comes from this message rather than from the device
     * list, so the object id is derived from the same bytes the discovery
     * payload was. Looking it up again would introduce a second source for
     * the same string and a rename would then silently split one entity into
     * two. */
    if (0 == EZPI_ha_item_object_id(object_id, sizeof(object_id),
                                    (NULL != cj_device_name) ? cj_device_name->valuestring : NULL,
                                    device_id, item_id))
    {
        cJSON_Delete(__FUNCTION__, cj_root);
        return EZPI_SUCCESS;
    }

    if (ezpi_mqtt_value_to_payload(cj_value, NULL, payload, sizeof(payload)) &&
        0 != EZPI_ha_topic_build(topic, sizeof(topic), CONFIG_EZPI_MQTT_TOPIC_PREFIX,
                                 sg_node_id, object_id, EZPI_MQTT_LEAF_STATE))
    {
        /* Retained so that a Home Assistant restart shows the last known
         * reading immediately instead of "unknown" until the sensor next
         * changes -- which for something like a door contact can be days.
         *
         * enqueue rather than publish: this is the loop-service task holding
         * the global serialization buffer, and publish() can wait on the
         * client. enqueue hands over to the MQTT task and returns. */
        esp_mqtt_client_enqueue(sg_client, topic, payload, (int)strlen(payload),
                                0, 1, false);
    }

    cJSON_Delete(__FUNCTION__, cj_root);

    return EZPI_SUCCESS;
}

/**
 * @brief Publish the discovery payload for one item, if it maps to an entity.
 */
static void ezpi_mqtt_publish_discovery_for_item(l_ezlopi_device_t *device,
                                                 l_ezlopi_item_t *item)
{
    s_ezpi_ha_entity_t entity;
    cJSON *cj_config = NULL;
    cJSON *cj_device = NULL;
    cJSON *cj_identifiers = NULL;
    char object_id[EZPI_MQTT_OBJECT_ID_LEN];
    char topic[EZPI_MQTT_TOPIC_LEN];
    char state_topic[EZPI_MQTT_TOPIC_LEN];
    char identifier[64];
    char *payload = NULL;
    const char *component = NULL;

    if (NULL == device || NULL == item)
    {
        return;
    }

    /* An item the hub does not show in its own UI is not something to put on
     * a Home Assistant dashboard either. */
    if (false == item->cloud_properties.show)
    {
        return;
    }

    if (false == EZPI_ha_describe(item->cloud_properties.item_name,
                                  item->cloud_properties.value_type,
                                  item->cloud_properties.scale,
                                  item->cloud_properties.has_setter,
                                  &entity))
    {
        return;
    }

    component = EZPI_ha_component_str(entity.component);
    if (NULL == component)
    {
        return;
    }

    if (0 == EZPI_ha_item_object_id(object_id, sizeof(object_id),
                                    device->cloud_properties.device_name,
                                    item->cloud_properties.device_id,
                                    item->cloud_properties.item_id))
    {
        return;
    }

    if (0 == EZPI_ha_topic_discovery(topic, sizeof(topic), CONFIG_EZPI_MQTT_DISCOVERY_PREFIX,
                                     component, sg_node_id, object_id) ||
        0 == EZPI_ha_topic_build(state_topic, sizeof(state_topic),
                                 CONFIG_EZPI_MQTT_TOPIC_PREFIX, sg_node_id, object_id,
                                 EZPI_MQTT_LEAF_STATE))
    {
        TRACE_E("mqtt: topics for '%s' do not fit", object_id);
        return;
    }

    cj_config = cJSON_CreateObject(__FUNCTION__);
    if (NULL == cj_config)
    {
        return;
    }

    /* The item name alone, not the device name with it. Home Assistant
     * prefixes the device name itself when it renders the entity, so
     * including it here produces "Kitchen Sensor Kitchen Sensor Temperature". */
    cJSON_AddStringToObject(__FUNCTION__, cj_config, ezlopi_name_str,
                            item->cloud_properties.item_name);
    cJSON_AddStringToObject(__FUNCTION__, cj_config, "unique_id", object_id);
    cJSON_AddStringToObject(__FUNCTION__, cj_config, "state_topic", state_topic);

    if (NULL != entity.device_class)
    {
        cJSON_AddStringToObject(__FUNCTION__, cj_config, "device_class", entity.device_class);
    }

    if (NULL != entity.unit)
    {
        cJSON_AddStringToObject(__FUNCTION__, cj_config, "unit_of_measurement", entity.unit);
    }

    if (NULL != entity.state_class)
    {
        cJSON_AddStringToObject(__FUNCTION__, cj_config, "state_class", entity.state_class);
    }

    if (EZPI_HA_COMPONENT_BINARY_SENSOR == entity.component ||
        EZPI_HA_COMPONENT_SWITCH == entity.component)
    {
        cJSON_AddStringToObject(__FUNCTION__, cj_config, "payload_on", EZPI_MQTT_TRUE_STR);
        cJSON_AddStringToObject(__FUNCTION__, cj_config, "payload_off", EZPI_MQTT_FALSE_STR);
    }

    if (EZPI_HA_COMPONENT_NUMBER == entity.component)
    {
        cJSON_AddNumberToObject(__FUNCTION__, cj_config, "min", entity.min);
        cJSON_AddNumberToObject(__FUNCTION__, cj_config, "max", entity.max);
    }

    /* A command topic only for the things that can actually be written. An
     * entity that advertises one but ignores what arrives is worse than a
     * read-only entity: it renders as a control the user can operate and
     * which then springs back. */
    if (EZPI_HA_COMPONENT_SWITCH == entity.component ||
        EZPI_HA_COMPONENT_NUMBER == entity.component)
    {
        char command_topic[EZPI_MQTT_TOPIC_LEN];

        if (0 != EZPI_ha_topic_build(command_topic, sizeof(command_topic),
                                     CONFIG_EZPI_MQTT_TOPIC_PREFIX, sg_node_id,
                                     object_id, EZPI_MQTT_LEAF_SET))
        {
            cJSON_AddStringToObject(__FUNCTION__, cj_config, "command_topic", command_topic);
        }
    }

    {
        char availability_topic[EZPI_MQTT_TOPIC_LEN];

        if (0 != EZPI_ha_topic_build(availability_topic, sizeof(availability_topic),
                                     CONFIG_EZPI_MQTT_TOPIC_PREFIX, sg_node_id, NULL,
                                     EZPI_MQTT_LEAF_STATUS))
        {
            cJSON_AddStringToObject(__FUNCTION__, cj_config, "availability_topic",
                                    availability_topic);
            cJSON_AddStringToObject(__FUNCTION__, cj_config, "payload_available",
                                    EZPI_MQTT_ONLINE_STR);
            cJSON_AddStringToObject(__FUNCTION__, cj_config, "payload_not_available",
                                    EZPI_MQTT_OFFLINE_STR);
        }
    }

    /* One Home Assistant device per EzloPi device, not per item and not one
     * for the whole hub. That is what makes a DHT22's temperature and
     * humidity appear as two entities of one device, which is how a user
     * expects to find them. */
    cj_device = cJSON_AddObjectToObject(__FUNCTION__, cj_config, "device");
    if (NULL != cj_device)
    {
        snprintf(identifier, sizeof(identifier), "%s_%08x", sg_node_id,
                 device->cloud_properties.device_id);

        cj_identifiers = cJSON_AddArrayToObject(__FUNCTION__, cj_device, "identifiers");
        if (NULL != cj_identifiers)
        {
            cJSON_AddItemToArray(cj_identifiers,
                                 cJSON_CreateString(__FUNCTION__, identifier));
        }

        cJSON_AddStringToObject(__FUNCTION__, cj_device, ezlopi_name_str,
                                device->cloud_properties.device_name);

        if (NULL != device->cloud_properties.subcategory)
        {
            cJSON_AddStringToObject(__FUNCTION__, cj_device, "model",
                                    device->cloud_properties.subcategory);
        }

        cJSON_AddStringToObject(__FUNCTION__, cj_device, "manufacturer", "EzloPi");
    }

    payload = cJSON_PrintUnformatted(__FUNCTION__, cj_config);
    if (NULL != payload)
    {
        /* Retained, which is what makes discovery work at all: Home Assistant
         * is usually not listening when a hub boots, and a retained config is
         * delivered whenever it next subscribes. */
        esp_mqtt_client_enqueue(sg_client, topic, payload, (int)strlen(payload),
                                0, 1, false);

        /* cjext allocates through ezlopi_malloc, so this must be the matching
         * ezlopi_free rather than the plain free() used elsewhere in the tree. */
        ezlopi_free(__FUNCTION__, payload);
    }

    cJSON_Delete(__FUNCTION__, cj_config);
}

/**
 * @brief Say we are here, then describe everything we have.
 */
static void ezpi_mqtt_announce(void)
{
    l_ezlopi_device_t *curr_device = NULL;
    char topic[EZPI_MQTT_TOPIC_LEN];
    char subscription[EZPI_MQTT_TOPIC_LEN];
    uint32_t published = 0;

    if (NULL == sg_client)
    {
        return;
    }

    /* Overwrites the retained "offline" the broker published on our behalf
     * last time the connection dropped. */
    if (0 != EZPI_ha_topic_build(topic, sizeof(topic), CONFIG_EZPI_MQTT_TOPIC_PREFIX,
                                 sg_node_id, NULL, EZPI_MQTT_LEAF_STATUS))
    {
        esp_mqtt_client_enqueue(sg_client, topic, EZPI_MQTT_ONLINE_STR,
                                (int)strlen(EZPI_MQTT_ONLINE_STR), 1, 1, false);
    }

    /* One wildcard subscription for every writable item, rather than one per
     * item. The ids are recovered from the topic, so a hub with sixty devices
     * still needs exactly one subscription, and adding a device later needs
     * no new one. */
    if (0 != EZPI_ha_topic_build(subscription, sizeof(subscription),
                                 CONFIG_EZPI_MQTT_TOPIC_PREFIX, sg_node_id, "+",
                                 EZPI_MQTT_LEAF_SET))
    {
        if (0 > esp_mqtt_client_subscribe(sg_client, subscription, 1))
        {
            TRACE_E("mqtt: subscribe to '%s' failed", subscription);
        }
    }

    curr_device = EZPI_core_device_get_head();
    while (NULL != curr_device)
    {
        l_ezlopi_item_t *curr_item = curr_device->items;

        while (NULL != curr_item)
        {
            ezpi_mqtt_publish_discovery_for_item(curr_device, curr_item);
            published++;
            curr_item = curr_item->next;
        }

        curr_device = curr_device->next;
    }

    TRACE_I("mqtt: announced, %u item(s) considered", published);
}

/**
 * @brief Act on a value written to an item's command topic.
 *
 * Reuses EZPI_items_set_value_v3 -- the same entry point the Ezlo cloud's
 * hub.item.value.set goes through -- by building the request it expects. The
 * alternative was a second copy of the device-list walk and the driver call,
 * which would then be the copy that does not get fixed when the real one is.
 */
static void ezpi_mqtt_handle_command(const char *topic, int topic_len,
                                     const char *data, int data_len)
{
    unsigned long device_id = 0;
    unsigned long item_id = 0;
    cJSON *cj_request = NULL;
    cJSON *cj_response = NULL;
    cJSON *cj_params = NULL;
    char id_str[16];
    char value_str[EZPI_MQTT_PAYLOAD_LEN];

    if (NULL == topic || 0 >= topic_len || NULL == data || 0 > data_len)
    {
        return;
    }

    if (false == EZPI_ha_parse_command_topic(topic, (size_t)topic_len, &device_id, &item_id))
    {
        TRACE_W("mqtt: ignoring a command on an unrecognised topic");
        return;
    }

    if ((size_t)data_len >= sizeof(value_str))
    {
        TRACE_W("mqtt: command payload too long (%d bytes)", data_len);
        return;
    }

    /* The payload is not NUL-terminated: esp-mqtt hands over a pointer and a
     * length into its own buffer. */
    memcpy(value_str, data, (size_t)data_len);
    value_str[data_len] = '\0';

    cj_request = cJSON_CreateObject(__FUNCTION__);
    cj_response = cJSON_CreateObject(__FUNCTION__);
    if (NULL == cj_request || NULL == cj_response)
    {
        cJSON_Delete(__FUNCTION__, cj_request);
        cJSON_Delete(__FUNCTION__, cj_response);
        return;
    }

    cj_params = cJSON_AddObjectToObject(__FUNCTION__, cj_request, ezlopi_params_str);
    if (NULL == cj_params)
    {
        cJSON_Delete(__FUNCTION__, cj_request);
        cJSON_Delete(__FUNCTION__, cj_response);
        return;
    }

    /* Eight hex digits with no 0x, which is what EZPI_core_cjson_get_id
     * parses and what the rest of the API uses on the wire. */
    snprintf(id_str, sizeof(id_str), "%08lx", item_id & 0xFFFFFFFFUL);
    cJSON_AddStringToObject(__FUNCTION__, cj_params, ezlopi__id_str, id_str);

    /* Drivers read params.value and accept a bool or a number, so the text
     * from the broker is turned into whichever the payload actually is. A
     * string here would be read as 0 by every driver in the tree, which would
     * turn every command into "off". */
    if (0 == strcmp(value_str, EZPI_MQTT_TRUE_STR) || 0 == strcmp(value_str, "ON") ||
        0 == strcmp(value_str, "on") || 0 == strcmp(value_str, "1"))
    {
        cJSON_AddBoolToObject(__FUNCTION__, cj_params, ezlopi_value_str, true);
    }
    else if (0 == strcmp(value_str, EZPI_MQTT_FALSE_STR) || 0 == strcmp(value_str, "OFF") ||
             0 == strcmp(value_str, "off") || 0 == strcmp(value_str, "0"))
    {
        cJSON_AddBoolToObject(__FUNCTION__, cj_params, ezlopi_value_str, false);
    }
    else
    {
        char *end = NULL;
        double number = strtod(value_str, &end);

        /* Only a payload that is entirely a number counts. Half-parsing
         * "42abc" as 42 would act on a message that is plainly not what the
         * sender meant. */
        if (NULL == end || end == value_str || '\0' != *end)
        {
            TRACE_W("mqtt: command payload '%s' is neither a bool nor a number", value_str);
            cJSON_Delete(__FUNCTION__, cj_request);
            cJSON_Delete(__FUNCTION__, cj_response);
            return;
        }

        cJSON_AddNumberToObject(__FUNCTION__, cj_params, ezlopi_value_str, number);
    }

    TRACE_I("mqtt: set item %08lx (device %08lx) to %s", item_id, device_id, value_str);

    EZPI_items_set_value_v3(cj_request, cj_response);

    cJSON_Delete(__FUNCTION__, cj_request);
    cJSON_Delete(__FUNCTION__, cj_response);

    /* No state is published here. The driver's own value-changed path
     * broadcasts, which arrives at ezpi_mqtt_on_broadcast and publishes the
     * value the hardware actually took. Echoing the requested value instead
     * would show a lamp as on when the relay failed to switch. */
}

static void ezpi_mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    (void)handler_args;
    (void)base;

    if (NULL == event)
    {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
    {
        TRACE_I("mqtt: connected");
        sg_connected = true;

        /* Re-announced on every reconnect, not just the first. A broker that
         * was restarted without persistence has forgotten every retained
         * config, and the hub is the only thing that can tell it again. */
        ezpi_mqtt_announce();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
    {
        TRACE_W("mqtt: disconnected");
        sg_connected = false;
        break;
    }
    case MQTT_EVENT_DATA:
    {
        ezpi_mqtt_handle_command(event->topic, event->topic_len,
                                 event->data, event->data_len);
        break;
    }
    case MQTT_EVENT_ERROR:
    {
        TRACE_E("mqtt: transport error");
        break;
    }
    default:
    {
        break;
    }
    }
}

#endif // CONFIG_EZPI_ENABLE_MQTT

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
