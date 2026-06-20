/*
 * Copyright (c) 2022 The ZMK Contributors
 * Copyright (c) 2026 The DYA Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT dya_analog_input

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk/drivers/dya_analog_input.h>

LOG_MODULE_REGISTER(dya_analog_input, CONFIG_DYA_ANALOG_INPUT_LOG_LEVEL);

static K_THREAD_STACK_DEFINE(dya_analog_input_q_stack,
                             CONFIG_DYA_ANALOG_INPUT_WORKQUEUE_STACK_SIZE);
static struct k_work_q dya_analog_input_work_q;
static bool work_q_started;

static const struct device *registered_devices[CONFIG_DYA_ANALOG_INPUT_MAX_DEVICES];
static uint8_t registered_device_count;

#if IS_ENABLED(CONFIG_DYA_ANALOG_INPUT_SETTINGS_PERSIST)
#define DYA_ANALOG_INPUT_SETTINGS_PREFIX "dya_analog_input"

struct dya_analog_input_axis_persisted {
    char name[16];
    uint8_t enabled;
    uint16_t adc_channel;
    uint16_t mv_mid;
    uint16_t mv_min_max;
    uint16_t mv_deadzone;
    uint16_t scale_multiplier;
    uint16_t scale_divisor;
    uint8_t invert;
    uint16_t role_evt_type;
    uint16_t role_input_code;
    uint8_t report_on_change_only;
    uint8_t output_min;
    uint8_t output_max;
    uint8_t response_curve;
};

struct dya_analog_input_device_persisted {
    uint8_t version;
    uint32_t sampling_hz;
    uint32_t report_interval_ms;
    struct dya_analog_input_axis_persisted axes[DYA_ANALOG_INPUT_MAX_AXES];
};

#define DYA_ANALOG_INPUT_SETTINGS_VERSION 1

static int settings_load_device_cb(const char *key, size_t len, settings_read_cb read_cb,
                                   void *cb_arg, void *param) {
    struct dya_analog_input_device_persisted *persisted = param;

    if (strcmp(key, "state") != 0 || len != sizeof(*persisted)) {
        return 0;
    }

    ssize_t rd = read_cb(cb_arg, persisted, sizeof(*persisted));
    if (rd < 0) {
        return (int)rd;
    }
    return 0;
}

static void apply_persisted_axis(struct dya_analog_input_axis_runtime_config *dst,
                                 const struct dya_analog_input_axis_persisted *src) {
    snprintf(dst->name, sizeof(dst->name), "%s", src->name);
    dst->enabled = src->enabled;
    dst->adc_channel.channel_id = src->adc_channel;
#if CONFIG_DYA_ANALOG_INPUT_USE_DTS_ADC_CH_CFG
    dst->adc_channel.channel_cfg.channel_id = src->adc_channel;
#endif
    dst->mv_mid = src->mv_mid;
    dst->mv_min_max = MAX(src->mv_min_max, 1U);
    dst->mv_deadzone = src->mv_deadzone;
    dst->scale_multiplier = MAX(src->scale_multiplier, 1U);
    dst->scale_divisor = MAX(src->scale_divisor, 1U);
    dst->invert = src->invert;
    dst->evt_type = src->role_evt_type;
    dst->input_code = src->role_input_code;
    dst->report_on_change_only = src->report_on_change_only;
    dst->output_min = src->output_min;
    dst->output_max = MAX(src->output_max, src->output_min + 1);
    dst->response_curve = (enum dya_analog_response_curve)src->response_curve;
}

static void sanitize_runtime_axis(struct dya_analog_input_axis_runtime_config *axis) {
    axis->scale_multiplier = MAX(axis->scale_multiplier, 1U);
    axis->scale_divisor = MAX(axis->scale_divisor, 1U);
    axis->mv_min_max = MAX(axis->mv_min_max, 1U);
    axis->output_max = MAX(axis->output_max, axis->output_min + 1U);
}

static void sanitize_runtime_device(struct dya_analog_input_data *data) {
    for (uint8_t i = 0; i < data->axes_len; i++) {
        sanitize_runtime_axis(&data->axes[i]);
    }
}

static void encode_persisted_axis(struct dya_analog_input_axis_persisted *dst,
                                  const struct dya_analog_input_axis_runtime_config *src) {
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->name, sizeof(dst->name), "%s", src->name);
    dst->enabled = src->enabled;
    dst->adc_channel = src->adc_channel.channel_id;
    dst->mv_mid = src->mv_mid;
    dst->mv_min_max = src->mv_min_max;
    dst->mv_deadzone = src->mv_deadzone;
    dst->scale_multiplier = src->scale_multiplier;
    dst->scale_divisor = src->scale_divisor;
    dst->invert = src->invert;
    dst->role_evt_type = src->evt_type;
    dst->role_input_code = src->input_code;
    dst->report_on_change_only = src->report_on_change_only;
    dst->output_min = src->output_min;
    dst->output_max = src->output_max;
    dst->response_curve = src->response_curve;
}

static int load_persisted_state(const struct device *dev) {
    struct dya_analog_input_data *data = dev->data;
    struct dya_analog_input_device_persisted persisted = {0};
    char subtree[40];

    snprintf(subtree, sizeof(subtree), "%s/%u", DYA_ANALOG_INPUT_SETTINGS_PREFIX, data->settings_id);
    int rc = settings_load_subtree_direct(subtree, settings_load_device_cb, &persisted);
    if (rc != 0) {
        LOG_WRN("%s failed to load persisted settings: %d", dev->name, rc);
        return rc;
    }

    if (persisted.version != DYA_ANALOG_INPUT_SETTINGS_VERSION) {
        return 0;
    }

    data->sampling_hz = persisted.sampling_hz;
    data->report_interval_ms = persisted.report_interval_ms;

    for (uint8_t i = 0; i < data->axes_len; i++) {
        apply_persisted_axis(&data->axes[i], &persisted.axes[i]);
    }
    sanitize_runtime_device(data);

    LOG_INF("%s restored persisted analog input settings", dev->name);
    return 0;
}

static int save_persisted_state(const struct device *dev) {
    struct dya_analog_input_data *data = dev->data;
    struct dya_analog_input_device_persisted persisted = {0};
    char key[48];

    persisted.version = DYA_ANALOG_INPUT_SETTINGS_VERSION;
    persisted.sampling_hz = data->sampling_hz;
    persisted.report_interval_ms = data->report_interval_ms;
    for (uint8_t i = 0; i < data->axes_len; i++) {
        encode_persisted_axis(&persisted.axes[i], &data->axes[i]);
    }

    snprintf(key, sizeof(key), "%s/%u/state", DYA_ANALOG_INPUT_SETTINGS_PREFIX, data->settings_id);
    int rc = settings_save_one(key, &persisted, sizeof(persisted));
    if (rc != 0) {
        LOG_WRN("%s failed to save persisted settings: %d", dev->name, rc);
    }
    return rc;
}
#endif

static uint32_t isqrt32(uint32_t value) {
    uint32_t result = 0;
    uint32_t bit = 1UL << 30;

    while (bit > value) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return result;
}

static uint32_t apply_curve_q15(uint32_t normalized_q15,
                                enum dya_analog_response_curve curve) {
    normalized_q15 = MIN(normalized_q15, 32768U);

    switch (curve) {
    case DYA_ANALOG_RESPONSE_CURVE_SOFT:
        return MIN(isqrt32(normalized_q15 * 32768U), 32768U);
    case DYA_ANALOG_RESPONSE_CURVE_AGGRESSIVE:
        return MIN((normalized_q15 * normalized_q15) / 32768U, 32768U);
    case DYA_ANALOG_RESPONSE_CURVE_LINEAR:
    default:
        return normalized_q15;
    }
}

static int32_t clamp_rel_for_hid(int32_t value, uint16_t evt_type) {
    if (evt_type != INPUT_EV_REL) {
        return value;
    }

    if (value > 127) {
        return 127;
    }
    if (value < -127) {
        return -127;
    }
    return value;
}

static int32_t axis_to_report_value(int32_t mv,
                                    const struct dya_analog_input_axis_runtime_config *axis) {
    if (!axis->enabled) {
        return 0;
    }

    int32_t delta = mv - axis->mv_mid;
    int32_t sign = delta >= 0 ? 1 : -1;
    uint32_t amount = (uint32_t)(delta >= 0 ? delta : -(int64_t)delta);

    if (amount <= axis->mv_deadzone) {
        return 0;
    }

    uint32_t active_range = axis->mv_min_max > axis->mv_deadzone
                                ? axis->mv_min_max - axis->mv_deadzone
                                : 1;
    uint32_t pushed = MIN(amount - axis->mv_deadzone, active_range);
    uint32_t normalized_q15 = (pushed * 32768U) / active_range;
    uint32_t curved_q15 = apply_curve_q15(normalized_q15, axis->response_curve);

    uint32_t output_min = axis->output_min;
    uint32_t output_max = MAX(axis->output_max, output_min + 1);
    uint32_t value = output_min + ((output_max - output_min) * curved_q15) / 32768U;

    value = (value * MAX(axis->scale_multiplier, 1U)) / MAX(axis->scale_divisor, 1U);

    int32_t signed_value = sign * (int32_t)value;
    int32_t result = axis->invert ? -signed_value : signed_value;
    return clamp_rel_for_hid(result, axis->evt_type);
}

static int rebuild_adc_sequence(const struct device *dev) {
    struct dya_analog_input_data *data = dev->data;
    uint32_t ch_mask = 0;

    for (uint8_t i = 0; i < data->axes_len; i++) {
        struct dya_analog_input_axis_runtime_config *axis = &data->axes[i];
        const struct device *adc = axis->adc_channel.dev;
        uint8_t channel_id = axis->adc_channel.channel_id;

#if CONFIG_DYA_ANALOG_INPUT_USE_DTS_ADC_CH_CFG
        struct adc_channel_cfg channel_cfg = axis->adc_channel.channel_cfg;
#else
        struct adc_channel_cfg channel_cfg = {
            .gain = ADC_GAIN_1_6,
            .reference = ADC_REF_INTERNAL,
            .acquisition_time = ADC_ACQ_TIME_DEFAULT,
            .channel_id = channel_id,
#ifdef CONFIG_ADC_CONFIGURABLE_INPUTS
#ifdef CONFIG_ADC_NRFX_SAADC
            .input_positive = SAADC_CH_PSELP_PSELP_AnalogInput0 + channel_id,
#else
            .input_positive = channel_id,
#endif
#endif
        };
#endif

        ch_mask |= BIT(channel_id);

        if (!device_is_ready(adc)) {
            LOG_ERR("Axis %u ADC device is not ready: %s", i, adc->name);
            return -ENODEV;
        }

        int err = adc_channel_setup(adc, &channel_cfg);
        if (err < 0) {
            LOG_ERR("Axis %u adc_channel_setup returned %d", i, err);
            return err;
        }
    }

    data->as.channels = ch_mask;
    data->as.calibrate = true;
    return 0;
}

static int dya_analog_input_report_data(const struct device *dev) {
    struct dya_analog_input_data *data = dev->data;

    if (unlikely(!data->ready)) {
        return -EBUSY;
    }

    int64_t now = k_uptime_get();
    struct adc_sequence *sequence = &data->as;

    if (data->axes_len == 0) {
        return 0;
    }

    const struct device *adc = data->axes[0].adc_channel.dev;

#ifdef CONFIG_ADC_ASYNC
    int err = adc_read_async(adc, sequence, &data->async_sig);
    if (err < 0) {
        LOG_ERR("adc_read_async returned %d", err);
        return err;
    }
    err = k_poll(&data->async_evt, 1, K_FOREVER);
    if (err < 0) {
        LOG_ERR("k_poll returned %d", err);
        return err;
    }
    if (!data->async_evt.signal->signaled) {
        return 0;
    }
    data->async_evt.signal->signaled = 0;
    data->async_evt.state = K_POLL_STATE_NOT_READY;
#else
    int err = adc_read(adc, sequence);
    if (err < 0) {
        LOG_ERR("adc_read returned %d", err);
        return err;
    }
#endif

    for (uint8_t i = 0; i < data->axes_len; i++) {
        const struct dya_analog_input_axis_runtime_config *axis = &data->axes[i];
        const struct device *axis_adc = axis->adc_channel.dev;
        int32_t mv = data->as_buff[i];
        int mv_rc = adc_raw_to_millivolts(adc_ref_internal(axis_adc), ADC_GAIN_1_6,
                                          sequence->resolution, &mv);
        if (mv_rc < 0) {
            LOG_WRN("axis %u adc_raw_to_millivolts failed: %d", i, mv_rc);
        }
        int32_t value = axis_to_report_value(mv, axis);

#if IS_ENABLED(CONFIG_DYA_ANALOG_INPUT_LOG_DBG_RAW)
        LOG_INF("axis %u adc %u raw %d mv %d", i, axis->adc_channel.channel_id,
                data->as_buff[i], mv);
#endif

        if (axis->report_on_change_only) {
            data->delta[i] = value;
        } else {
            data->delta[i] += value;
        }
    }

    data->as.calibrate = false;

    if (data->report_interval_ms > 0 &&
        now - data->last_sample_time >= CONFIG_DYA_ANALOG_INPUT_STALE_SAMPLE_MS) {
        for (uint8_t i = 0; i < data->axes_len; i++) {
            data->delta[i] = 0;
            data->prev[i] = 0;
        }
    }
    data->last_sample_time = now;

    if (data->report_interval_ms > 0 && now - data->last_report_time < data->report_interval_ms) {
        return 0;
    }

    if (!data->active) {
        return 0;
    }

    int8_t idx_to_sync = -1;
    for (int8_t i = data->axes_len - 1; i >= 0; i--) {
        if (data->delta[i] != data->prev[i]) {
            idx_to_sync = i;
            break;
        }
    }

    if (idx_to_sync < 0) {
        return 0;
    }

    for (uint8_t i = 0; i < data->axes_len; i++) {
        const struct dya_analog_input_axis_runtime_config *axis = &data->axes[i];
        int32_t value = clamp_rel_for_hid(data->delta[i], axis->evt_type);
        int32_t previous = data->prev[i];

        if (value == previous) {
            continue;
        }

        data->delta[i] = 0;
        if (axis->report_on_change_only) {
            data->prev[i] = value;
        }

#if IS_ENABLED(CONFIG_DYA_ANALOG_INPUT_LOG_DBG_REPORT)
        LOG_INF("input_report axis %u value %d type %u code %u", i, value, axis->evt_type,
                axis->input_code);
#endif
        input_report(dev, axis->evt_type, axis->input_code, value, i == idx_to_sync, K_NO_WAIT);
    }

    data->last_report_time = now;
    return 0;
}
static void sampling_work_handler(struct k_work *work) {
    struct dya_analog_input_data *data = CONTAINER_OF(work, struct dya_analog_input_data,
                                                     sampling_work);
#if IS_ENABLED(CONFIG_DYA_ANALOG_INPUT_LOG_DBG_RAW)
    LOG_INF("%s sampling work", data->dev->name);
#endif
    (void)dya_analog_input_report_data(data->dev);
}

static void sampling_timer_handler(struct k_timer *timer) {
    struct dya_analog_input_data *data = CONTAINER_OF(timer, struct dya_analog_input_data,
                                                     sampling_timer);
#if IS_ENABLED(CONFIG_DYA_ANALOG_INPUT_LOG_DBG_RAW)
    LOG_INF("%s sampling timer", data->dev->name);
#endif
    k_work_submit_to_queue(&dya_analog_input_work_q, &data->sampling_work);
}

static void stop_sampling_sync(struct dya_analog_input_data *data) {
    k_timer_stop(&data->sampling_timer);
    k_work_cancel_sync(&data->sampling_work, &data->sampling_work_sync);
}

static int active_set_value(const struct device *dev, bool active) {
    struct dya_analog_input_data *data = dev->data;
    data->active = active;
    return 0;
}

static int enable_set_value(const struct device *dev, bool enable) {
    struct dya_analog_input_data *data = dev->data;

    if (unlikely(!data->ready)) {
        return -EBUSY;
    }

    if (data->enabled == enable) {
        return 0;
    }

    if (enable) {
        if (data->sampling_hz == 0) {
            k_timer_start(&data->sampling_timer, K_NO_WAIT, K_NO_WAIT);
            LOG_INF("%s sampling enabled: one-shot", dev->name);
        } else {
            uint32_t usec = 1000000UL / data->sampling_hz;
            k_timer_start(&data->sampling_timer, K_USEC(usec), K_USEC(usec));
            LOG_INF("%s sampling enabled: %u Hz", dev->name, data->sampling_hz);
        }
    } else {
        stop_sampling_sync(data);
        LOG_INF("%s sampling disabled", dev->name);
    }

    data->enabled = enable;
    return 0;
}

static int sample_hz_set_value(const struct device *dev, uint32_t hz) {
    struct dya_analog_input_data *data = dev->data;
    bool was_enabled = data->enabled;

    if (unlikely(!data->ready)) {
        data->sampling_hz = hz;
        return 0;
    }

    if (was_enabled) {
        enable_set_value(dev, false);
    }
    data->sampling_hz = hz;
    if (was_enabled) {
        enable_set_value(dev, true);
    }

    return 0;
}

static void copy_defaults_to_runtime(const struct device *dev) {
    const struct dya_analog_input_config *config = dev->config;
    struct dya_analog_input_data *data = dev->data;

    data->axes_len = MIN(config->axes_len, DYA_ANALOG_INPUT_MAX_AXES);
    data->sampling_hz = config->sampling_hz;
    data->report_interval_ms = config->report_interval_ms;

    for (uint8_t i = 0; i < data->axes_len; i++) {
        const struct dya_analog_input_axis_config *src = &config->axes[i];
        struct dya_analog_input_axis_runtime_config *dst = &data->axes[i];

        dst->adc_channel = src->adc_channel;
        snprintf(dst->name, sizeof(dst->name), "%s", src->label ? src->label : (i == 0 ? "X Axis" : "Y Axis"));
        dst->enabled = true;
        dst->mv_mid = src->mv_mid;
        dst->mv_min_max = src->mv_min_max;
        dst->mv_deadzone = src->mv_deadzone;
        dst->invert = src->invert;
        dst->report_on_change_only = src->report_on_change_only;
        dst->scale_multiplier = src->scale_multiplier;
        dst->scale_divisor = src->scale_divisor;
        dst->evt_type = src->evt_type;
        dst->input_code = src->input_code;
        dst->output_min = src->output_min;
        dst->output_max = src->output_max;
        dst->response_curve = src->response_curve;
        sanitize_runtime_axis(dst);
    }
}

static void dya_analog_input_async_init(struct k_work *work) {
    struct k_work_delayable *work_delayable = (struct k_work_delayable *)work;
    struct dya_analog_input_data *data = CONTAINER_OF(work_delayable,
                                                     struct dya_analog_input_data, init_work);
    const struct device *dev = data->dev;

    copy_defaults_to_runtime(dev);
#if IS_ENABLED(CONFIG_DYA_ANALOG_INPUT_SETTINGS_PERSIST)
    (void)load_persisted_state(dev);
#endif
    if (data->axes_len == 0) {
        LOG_ERR("%s has no analog axes", dev->name);
        return;
    }

    LOG_INF("%s initializing: axes=%u sampling=%uHz report=%ums", dev->name, data->axes_len,
            data->sampling_hz, data->report_interval_ms);
    for (uint8_t i = 0; i < data->axes_len; i++) {
        LOG_INF("%s axis %u: adc=%s channel=%u mid=%u range=%u deadzone=%u type=%u code=%u",
                dev->name, i, data->axes[i].adc_channel.dev->name,
                data->axes[i].adc_channel.channel_id, data->axes[i].mv_mid,
                data->axes[i].mv_min_max, data->axes[i].mv_deadzone, data->axes[i].evt_type,
                data->axes[i].input_code);
    }

    memset(data->delta, 0, sizeof(data->delta));
    memset(data->prev, 0, sizeof(data->prev));
    memset(data->as_buff, 0, sizeof(data->as_buff));

    data->as = (struct adc_sequence){
        .buffer = data->as_buff,
        .buffer_size = data->axes_len * sizeof(data->as_buff[0]),
        .oversampling = 0,
        .resolution = CONFIG_DYA_ANALOG_INPUT_ADC_RES,
        .calibrate = true,
    };

    if (rebuild_adc_sequence(dev) < 0) {
        return;
    }

#ifdef CONFIG_ADC_ASYNC
    k_poll_signal_init(&data->async_sig);
    struct k_poll_event async_evt = K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL,
                                                             K_POLL_MODE_NOTIFY_ONLY,
                                                             &data->async_sig);
    data->async_evt = async_evt;
#endif

    if (!work_q_started) {
        k_work_queue_start(&dya_analog_input_work_q, dya_analog_input_q_stack,
                           K_THREAD_STACK_SIZEOF(dya_analog_input_q_stack),
                           CONFIG_DYA_ANALOG_INPUT_WORKQUEUE_PRIORITY, NULL);
        work_q_started = true;
    }

    k_work_init(&data->sampling_work, sampling_work_handler);
    k_timer_init(&data->sampling_timer, sampling_timer_handler, NULL);

    data->ready = true;
    active_set_value(dev, true);
    if (data->sampling_hz) {
        enable_set_value(dev, true);
    }
    k_work_submit_to_queue(&dya_analog_input_work_q, &data->sampling_work);
    LOG_INF("%s ready", dev->name);
}

static int dya_analog_input_init(const struct device *dev) {
    struct dya_analog_input_data *data = dev->data;
    data->dev = dev;
    LOG_INF("%s scheduled init", dev->name);

    if (registered_device_count < ARRAY_SIZE(registered_devices)) {
        data->settings_id = registered_device_count;
        registered_devices[registered_device_count++] = dev;
    } else {
        LOG_WRN("Too many dya analog input devices; Studio will only see the first %u",
                ARRAY_SIZE(registered_devices));
    }

    k_work_init_delayable(&data->init_work, dya_analog_input_async_init);
    k_work_schedule(&data->init_work, K_MSEC(1));
    return 0;
}

static int dya_analog_input_attr_set(const struct device *dev, enum sensor_channel chan,
                                     enum sensor_attribute attr, const struct sensor_value *val) {
    if (chan != SENSOR_CHAN_ALL) {
        return -ENOTSUP;
    }

    switch ((uint32_t)attr) {
    case DYA_ANALOG_INPUT_ATTR_SAMPLING_HZ:
        return sample_hz_set_value(dev, DYA_ANALOG_INPUT_SVALUE_TO_SAMPLING_HZ(*val));
    case DYA_ANALOG_INPUT_ATTR_ENABLE:
        return enable_set_value(dev, DYA_ANALOG_INPUT_SVALUE_TO_ENABLE(*val));
    case DYA_ANALOG_INPUT_ATTR_ACTIVE:
        return active_set_value(dev, DYA_ANALOG_INPUT_SVALUE_TO_ACTIVE(*val));
    default:
        return -ENOTSUP;
    }
}

static int dya_analog_input_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    if (chan != SENSOR_CHAN_ALL) {
        return -ENOTSUP;
    }

    return dya_analog_input_report_data(dev);
}

static int dya_analog_input_channel_get(const struct device *dev, enum sensor_channel chan,
                                        struct sensor_value *val) {
    struct dya_analog_input_data *data = dev->data;

    if (chan != SENSOR_CHAN_ALL || unlikely(!data->ready)) {
        return -ENOTSUP;
    }

    val->val1 = data->axes_len > 0 ? data->delta[0] : 0;
    val->val2 = data->axes_len > 1 ? data->delta[1] : 0;
    return 0;
}

static const struct sensor_driver_api dya_analog_input_driver_api = {
    .attr_set = dya_analog_input_attr_set,
    .sample_fetch = dya_analog_input_sample_fetch,
    .channel_get = dya_analog_input_channel_get,
};

uint8_t dya_analog_input_device_count(void) { return registered_device_count; }

const struct device *dya_analog_input_device_get(uint8_t id) {
    return id < registered_device_count ? registered_devices[id] : NULL;
}

int dya_analog_input_runtime_reset(const struct device *dev) {
    struct dya_analog_input_data *data = dev->data;
    bool was_enabled = data->enabled;

    if (unlikely(!data->ready)) {
        return -EBUSY;
    }

    if (was_enabled) {
        enable_set_value(dev, false);
    } else {
        stop_sampling_sync(data);
    }
    copy_defaults_to_runtime(dev);
    int err = rebuild_adc_sequence(dev);
    memset(data->delta, 0, data->axes_len * sizeof(int32_t));
    memset(data->prev, 0, data->axes_len * sizeof(int32_t));
    if (was_enabled) {
        enable_set_value(dev, true);
    }
#if IS_ENABLED(CONFIG_DYA_ANALOG_INPUT_SETTINGS_PERSIST)
    (void)save_persisted_state(dev);
#endif
    return err;
}

int dya_analog_input_runtime_set_sampling_hz(const struct device *dev, uint32_t hz) {
    int rc = sample_hz_set_value(dev, hz);
#if IS_ENABLED(CONFIG_DYA_ANALOG_INPUT_SETTINGS_PERSIST)
    if (rc == 0) {
        (void)save_persisted_state(dev);
    }
#endif
    return rc;
}

int dya_analog_input_runtime_set_report_interval_ms(const struct device *dev,
                                                    uint32_t interval_ms) {
    struct dya_analog_input_data *data = dev->data;

    if (unlikely(!data->ready)) {
        return -EBUSY;
    }

    data->report_interval_ms = interval_ms;
#if IS_ENABLED(CONFIG_DYA_ANALOG_INPUT_SETTINGS_PERSIST)
    (void)save_persisted_state(dev);
#endif
    return 0;
}

int dya_analog_input_runtime_set_axis(const struct device *dev, uint8_t axis_index,
                                      const struct dya_analog_input_axis_runtime_config *axis) {
    struct dya_analog_input_data *data = dev->data;

    if (axis_index >= data->axes_len) {
        return -EINVAL;
    }

    if (unlikely(!data->ready)) {
        return -EBUSY;
    }

    bool was_enabled = data->enabled;
    if (was_enabled) {
        enable_set_value(dev, false);
    } else {
        stop_sampling_sync(data);
    }

    data->axes[axis_index] = *axis;
    sanitize_runtime_axis(&data->axes[axis_index]);

    int err = rebuild_adc_sequence(dev);
    data->delta[axis_index] = 0;
    data->prev[axis_index] = 0;

    if (was_enabled) {
        enable_set_value(dev, true);
    }

#if IS_ENABLED(CONFIG_DYA_ANALOG_INPUT_SETTINGS_PERSIST)
    if (err == 0) {
        (void)save_persisted_state(dev);
    }
#endif
    return err;
}

#define AXIS_COUNT_PLUS_ONE(node) +1

#define DYA_ANALOG_INPUT_AXIS_ENTRY(node_id)                                                       \
    {                                                                                              \
        .adc_channel = ADC_DT_SPEC_GET_BY_IDX(node_id, 0),                                         \
        .label = DT_PROP_OR(node_id, label, NULL),                                                 \
        .mv_mid = DT_PROP(node_id, mv_mid),                                                        \
        .mv_min_max = DT_PROP(node_id, mv_min_max),                                                \
        .mv_deadzone = DT_PROP(node_id, mv_deadzone),                                              \
        .invert = DT_PROP(node_id, invert),                                                        \
        .report_on_change_only = DT_PROP(node_id, report_on_change_only),                          \
        .scale_multiplier = DT_PROP(node_id, scale_multiplier),                                    \
        .scale_divisor = DT_PROP(node_id, scale_divisor),                                          \
        .evt_type = DT_PROP(node_id, evt_type),                                                    \
        .input_code = DT_PROP(node_id, input_code),                                                \
        .output_min = DT_PROP(node_id, output_min),                                                \
        .output_max = DT_PROP(node_id, output_max),                                                \
        .response_curve = DT_PROP(node_id, response_curve),                                        \
    }

#define DYA_ANALOG_INPUT_DEFINE(n)                                                                 \
    BUILD_ASSERT((0 DT_FOREACH_CHILD(DT_DRV_INST(n), AXIS_COUNT_PLUS_ONE)) <=                      \
                     DYA_ANALOG_INPUT_MAX_AXES,                                                    \
                 "dya,analog-input supports at most two axes for Studio RPC");                     \
    static struct dya_analog_input_data data##n = {};                                              \
    static const struct dya_analog_input_config config##n = {                                      \
        .sampling_hz = DT_PROP(DT_DRV_INST(n), sampling_hz),                                      \
        .report_interval_ms = DT_PROP(DT_DRV_INST(n), report_interval_ms),                        \
        .axes_len = (0 DT_FOREACH_CHILD(DT_DRV_INST(n), AXIS_COUNT_PLUS_ONE)),                    \
        .axes = {DT_INST_FOREACH_CHILD_SEP(n, DYA_ANALOG_INPUT_AXIS_ENTRY, (,))},                 \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, dya_analog_input_init, NULL, &data##n, &config##n, POST_KERNEL,       \
                          CONFIG_SENSOR_INIT_PRIORITY, &dya_analog_input_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DYA_ANALOG_INPUT_DEFINE)
