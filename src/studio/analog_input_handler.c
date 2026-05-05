/*
 * Copyright (c) 2026 The DYA Contributors
 * SPDX-License-Identifier: MIT
 */

#include <pb_decode.h>
#include <pb_encode.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zmk/drivers/dya_analog_input.h>
#include <zmk/studio/custom.h>

#include <dya/analog_input/analog_input.pb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta dya_analog_input_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(""),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(dya_analog_input, &dya_analog_input_meta,
                         dya_analog_input_rpc_handle_request);
ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(dya_analog_input, dya_analog_input_Response);

static dya_analog_input_AnalogRole role_from_input(uint16_t type, uint16_t code) {
    if (type == INPUT_EV_REL && code == INPUT_REL_X) {
        return dya_analog_input_AnalogRole_ANALOG_ROLE_REL_X;
    }
    if (type == INPUT_EV_REL && code == INPUT_REL_Y) {
        return dya_analog_input_AnalogRole_ANALOG_ROLE_REL_Y;
    }
    if (type == INPUT_EV_REL && code == INPUT_REL_WHEEL) {
        return dya_analog_input_AnalogRole_ANALOG_ROLE_REL_WHEEL;
    }
    if (type == INPUT_EV_REL && code == INPUT_REL_HWHEEL) {
        return dya_analog_input_AnalogRole_ANALOG_ROLE_REL_HWHEEL;
    }
    if (type == INPUT_EV_ABS && code == INPUT_ABS_X) {
        return dya_analog_input_AnalogRole_ANALOG_ROLE_ABS_X;
    }
    if (type == INPUT_EV_ABS && code == INPUT_ABS_Y) {
        return dya_analog_input_AnalogRole_ANALOG_ROLE_ABS_Y;
    }
    return dya_analog_input_AnalogRole_ANALOG_ROLE_REL_X;
}

static void input_from_role(dya_analog_input_AnalogRole role, uint16_t *type, uint16_t *code) {
    switch (role) {
    case dya_analog_input_AnalogRole_ANALOG_ROLE_REL_Y:
        *type = INPUT_EV_REL;
        *code = INPUT_REL_Y;
        break;
    case dya_analog_input_AnalogRole_ANALOG_ROLE_REL_WHEEL:
        *type = INPUT_EV_REL;
        *code = INPUT_REL_WHEEL;
        break;
    case dya_analog_input_AnalogRole_ANALOG_ROLE_REL_HWHEEL:
        *type = INPUT_EV_REL;
        *code = INPUT_REL_HWHEEL;
        break;
    case dya_analog_input_AnalogRole_ANALOG_ROLE_ABS_X:
        *type = INPUT_EV_ABS;
        *code = INPUT_ABS_X;
        break;
    case dya_analog_input_AnalogRole_ANALOG_ROLE_ABS_Y:
        *type = INPUT_EV_ABS;
        *code = INPUT_ABS_Y;
        break;
    case dya_analog_input_AnalogRole_ANALOG_ROLE_REL_X:
    default:
        *type = INPUT_EV_REL;
        *code = INPUT_REL_X;
        break;
    }
}

static int fill_axis(dya_analog_input_AnalogAxisConfig *dst,
                     const struct dya_analog_input_axis_runtime_config *src, uint8_t index) {
    *dst = (dya_analog_input_AnalogAxisConfig)dya_analog_input_AnalogAxisConfig_init_zero;
    dst->axis_index = index;
    snprintf(dst->name, sizeof(dst->name), "%s", src->name);
    dst->enabled = src->enabled;
    dst->adc_channel = src->adc_channel.channel_id;
    dst->mv_mid = src->mv_mid;
    dst->mv_min_max = src->mv_min_max;
    dst->mv_deadzone = src->mv_deadzone;
    dst->scale_multiplier = src->scale_multiplier;
    dst->scale_divisor = src->scale_divisor;
    dst->invert = src->invert;
    dst->role = role_from_input(src->evt_type, src->input_code);
    dst->report_on_change_only = src->report_on_change_only;
    dst->output_min = src->output_min;
    dst->output_max = src->output_max;
    dst->response_curve = (dya_analog_input_AnalogResponseCurve)src->response_curve;
    return 0;
}

static int fill_device(dya_analog_input_AnalogInputDevice *dst, uint8_t id,
                       const struct device *dev) {
    struct dya_analog_input_data *data = dev->data;

    if (!data->ready) {
        return -EBUSY;
    }

    *dst = (dya_analog_input_AnalogInputDevice)dya_analog_input_AnalogInputDevice_init_zero;
    dst->id = id;
    snprintf(dst->name, sizeof(dst->name), "%s", dev->name);
    dst->sampling_hz = data->sampling_hz;
    dst->report_interval_ms = data->report_interval_ms;
    dst->axes_count = data->axes_len;

    for (uint8_t i = 0; i < data->axes_len && i < ARRAY_SIZE(dst->axes); i++) {
        fill_axis(&dst->axes[i], &data->axes[i], i);
    }

    return 0;
}

static const struct device *get_device_or_error(uint32_t id, dya_analog_input_Response *resp) {
    const struct device *dev = dya_analog_input_device_get(id);
    if (dev != NULL) {
        return dev;
    }

    resp->which_response_type = dya_analog_input_Response_error_tag;
    snprintf(resp->response_type.error.message, sizeof(resp->response_type.error.message),
             "Analog device not found: %u", id);
    return NULL;
}

static int handle_list_devices(dya_analog_input_Response *resp) {
    dya_analog_input_ListDevicesResponse result = dya_analog_input_ListDevicesResponse_init_zero;
    uint8_t count = MIN(dya_analog_input_device_count(), ARRAY_SIZE(result.devices));

    result.devices_count = count;
    for (uint8_t i = 0; i < count; i++) {
        const struct device *dev = dya_analog_input_device_get(i);
        if (dev != NULL) {
            fill_device(&result.devices[i], i, dev);
        }
    }

    resp->which_response_type = dya_analog_input_Response_list_devices_tag;
    resp->response_type.list_devices = result;
    return 0;
}

static int handle_get_device(const dya_analog_input_GetDeviceRequest *req,
                             dya_analog_input_Response *resp) {
    const struct device *dev = get_device_or_error(req->id, resp);
    if (dev == NULL) {
        return -ENOENT;
    }

    dya_analog_input_GetDeviceResponse result = dya_analog_input_GetDeviceResponse_init_zero;
    fill_device(&result.device, req->id, dev);

    resp->which_response_type = dya_analog_input_Response_get_device_tag;
    resp->response_type.get_device = result;
    return 0;
}

static int handle_set_sampling_hz(const dya_analog_input_SetSamplingHzRequest *req,
                                  dya_analog_input_Response *resp) {
    const struct device *dev = get_device_or_error(req->id, resp);
    if (dev == NULL) {
        return -ENOENT;
    }

    int rc = dya_analog_input_runtime_set_sampling_hz(dev, req->value);
    if (rc == 0) {
        resp->which_response_type = dya_analog_input_Response_set_sampling_hz_tag;
        resp->response_type.set_sampling_hz =
            (dya_analog_input_SetSamplingHzResponse)dya_analog_input_SetSamplingHzResponse_init_zero;
    }
    return rc;
}

static int handle_set_report_interval(const dya_analog_input_SetReportIntervalRequest *req,
                                      dya_analog_input_Response *resp) {
    const struct device *dev = get_device_or_error(req->id, resp);
    if (dev == NULL) {
        return -ENOENT;
    }

    int rc = dya_analog_input_runtime_set_report_interval_ms(dev, req->value_ms);
    if (rc == 0) {
        resp->which_response_type = dya_analog_input_Response_set_report_interval_tag;
        resp->response_type.set_report_interval =
            (dya_analog_input_SetReportIntervalResponse)
                dya_analog_input_SetReportIntervalResponse_init_zero;
    }
    return rc;
}

static int handle_set_axis_config(const dya_analog_input_SetAxisConfigRequest *req,
                                  dya_analog_input_Response *resp) {
    const struct device *dev = get_device_or_error(req->device_id, resp);
    if (dev == NULL) {
        return -ENOENT;
    }

    struct dya_analog_input_data *data = dev->data;
    uint8_t index = req->axis.axis_index;
    if (index >= data->axes_len) {
        resp->which_response_type = dya_analog_input_Response_error_tag;
        snprintf(resp->response_type.error.message, sizeof(resp->response_type.error.message),
                 "Analog axis not found: %u", index);
        return -ENOENT;
    }

    struct dya_analog_input_axis_runtime_config axis = data->axes[index];
    snprintf(axis.name, sizeof(axis.name), "%s", req->axis.name);
    axis.enabled = req->axis.enabled;
    axis.adc_channel.channel_id = req->axis.adc_channel;
#if CONFIG_DYA_ANALOG_INPUT_USE_DTS_ADC_CH_CFG
    axis.adc_channel.channel_cfg.channel_id = req->axis.adc_channel;
#endif
    axis.mv_mid = req->axis.mv_mid;
    axis.mv_min_max = req->axis.mv_min_max;
    axis.mv_deadzone = req->axis.mv_deadzone;
    axis.scale_multiplier = req->axis.scale_multiplier;
    axis.scale_divisor = req->axis.scale_divisor;
    axis.invert = req->axis.invert;
    input_from_role(req->axis.role, &axis.evt_type, &axis.input_code);
    axis.report_on_change_only = req->axis.report_on_change_only;
    axis.output_min = req->axis.output_min;
    axis.output_max = req->axis.output_max;
    axis.response_curve = (enum dya_analog_response_curve)req->axis.response_curve;

    int rc = dya_analog_input_runtime_set_axis(dev, index, &axis);
    if (rc == 0) {
        resp->which_response_type = dya_analog_input_Response_set_axis_config_tag;
        resp->response_type.set_axis_config =
            (dya_analog_input_SetAxisConfigResponse)
                dya_analog_input_SetAxisConfigResponse_init_zero;
    }
    return rc;
}

static int handle_reset_device(const dya_analog_input_ResetDeviceRequest *req,
                               dya_analog_input_Response *resp) {
    const struct device *dev = get_device_or_error(req->id, resp);
    if (dev == NULL) {
        return -ENOENT;
    }

    int rc = dya_analog_input_runtime_reset(dev);
    if (rc == 0) {
        resp->which_response_type = dya_analog_input_Response_reset_device_tag;
        resp->response_type.reset_device =
            (dya_analog_input_ResetDeviceResponse)dya_analog_input_ResetDeviceResponse_init_zero;
    }
    return rc;
}

static bool dya_analog_input_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                                pb_callback_t *encode_response) {
    dya_analog_input_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(dya_analog_input, encode_response);
    dya_analog_input_Request req = dya_analog_input_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, dya_analog_input_Request_fields, &req)) {
        resp->which_response_type = dya_analog_input_Response_error_tag;
        snprintf(resp->response_type.error.message, sizeof(resp->response_type.error.message),
                 "Failed to decode request");
        LOG_WRN("Failed to decode dya analog input request: %s", PB_GET_ERROR(&req_stream));
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case dya_analog_input_Request_list_devices_tag:
        rc = handle_list_devices(resp);
        break;
    case dya_analog_input_Request_get_device_tag:
        rc = handle_get_device(&req.request_type.get_device, resp);
        break;
    case dya_analog_input_Request_set_sampling_hz_tag:
        rc = handle_set_sampling_hz(&req.request_type.set_sampling_hz, resp);
        break;
    case dya_analog_input_Request_set_report_interval_tag:
        rc = handle_set_report_interval(&req.request_type.set_report_interval, resp);
        break;
    case dya_analog_input_Request_set_axis_config_tag:
        rc = handle_set_axis_config(&req.request_type.set_axis_config, resp);
        break;
    case dya_analog_input_Request_reset_device_tag:
        rc = handle_reset_device(&req.request_type.reset_device, resp);
        break;
    default:
        rc = -ENOTSUP;
        break;
    }

    if (rc != 0 && resp->which_response_type != dya_analog_input_Response_error_tag) {
        resp->which_response_type = dya_analog_input_Response_error_tag;
        snprintf(resp->response_type.error.message, sizeof(resp->response_type.error.message),
                 "Failed to process request: %d", rc);
    }

    return true;
}
