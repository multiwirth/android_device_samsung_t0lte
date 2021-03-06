/*
 * Copyright (C) 2013 Paul Kocialkowski <contact@paulk.fr>
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

#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <linux/ioctl.h>
#include <linux/input.h>

#include <hardware/sensors.h>
#include <hardware/hardware.h>

#define LOG_TAG "smdk4x12_sensors"
#include <utils/Log.h>

#include "smdk4x12_sensors.h"
#include "ssp.h"

struct bmp180_data {
	char path_delay[PATH_MAX];
};

int bmp180_init(struct smdk4x12_sensors_handlers *handlers,
	struct smdk4x12_sensors_device *device)
{
	struct bmp180_data *data = NULL;
	char path[PATH_MAX] = { 0 };
	int input_fd = -1;
	int rc;

	ALOGD("%s(%p, %p)", __func__, handlers, device);

	if (handlers == NULL)
		return -EINVAL;

	data = (struct bmp180_data *) calloc(1, sizeof(struct bmp180_data));

	input_fd = input_open("pressure_sensor");
	if (input_fd < 0) {
		ALOGE("%s: Unable to open input", __func__);
		goto error;
	}

	rc = sysfs_path_prefix("pressure_sensor", (char *) &path);
	if (rc < 0 || path[0] == '\0') {
		ALOGE("%s: Unable to open sysfs", __func__);
		goto error;
	}

	snprintf(data->path_delay, PATH_MAX, "%s/poll_delay", path);

	handlers->poll_fd = input_fd;
	handlers->data = (void *) data;

	return 0;

error:
	if (data != NULL)
		free(data);

	if (input_fd >= 0)
		close(input_fd);

	handlers->poll_fd = -1;
	handlers->data = NULL;

	return -1;
}

int bmp180_deinit(struct smdk4x12_sensors_handlers *handlers)
{
	ALOGD("%s(%p)", __func__, handlers);

	if (handlers == NULL)
		return -EINVAL;

	if (handlers->poll_fd >= 0)
		close(handlers->poll_fd);
	handlers->poll_fd = -1;

	if (handlers->data != NULL)
		free(handlers->data);
	handlers->data = NULL;

	return 0;
}

int bmp180_set_delay(struct smdk4x12_sensors_handlers *handlers, int64_t delay);

static void* set_initial_state_fn(void *data) {
	struct smdk4x12_sensors_handlers *handlers = (struct smdk4x12_sensors_handlers*)data;

	ALOGE("%s: start", __func__);
	usleep(100000); // 100ms
	if (handlers == NULL || handlers->data == NULL)
		return NULL;

	bmp180_set_delay(handlers, 100000);
	ALOGE("%s: end", __func__);

	return NULL;
}

static void set_initial_state_thread(struct smdk4x12_sensors_handlers *handlers) {
	pthread_attr_t thread_attr;
	pthread_t setdelay_thread;

	pthread_attr_init(&thread_attr);
	pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&setdelay_thread, &thread_attr, set_initial_state_fn, (void*)handlers);
	if (rc < 0)
		ALOGE("%s: Unable to create thread", __func__);
}

int bmp180_activate(struct smdk4x12_sensors_handlers *handlers)
{
	struct bmp180_data *data;
	int rc;

	ALOGD("%s(%p)", __func__, handlers);

	if (handlers == NULL || handlers->data == NULL)
		return -EINVAL;

	data = (struct bmp180_data *) handlers->data;

	rc = ssp_sensor_enable(PRESSURE_SENSOR);
	if (rc < 0) {
		ALOGE("%s: Unable to enable ssp sensor", __func__);
		return -1;
	}

	handlers->activated = 1;
	set_initial_state_thread(handlers);

	return 0;
}

int bmp180_deactivate(struct smdk4x12_sensors_handlers *handlers)
{
	struct bmp180_data *data;
	int rc;

	ALOGD("%s(%p)", __func__, handlers);

	if (handlers == NULL || handlers->data == NULL)
		return -EINVAL;

	data = (struct bmp180_data *) handlers->data;

	rc = ssp_sensor_disable(PRESSURE_SENSOR);
	if (rc < 0) {
		ALOGE("%s: Unable to disable ssp sensor", __func__);
		return -1;
	}

	handlers->activated = 1;

	return 0;
}

int bmp180_set_delay(struct smdk4x12_sensors_handlers *handlers, int64_t delay)
{
	struct bmp180_data *data;
	int rc;

	ALOGD("%s(%p, %" PRId64 ")", __func__, handlers, delay);

	if (handlers == NULL || handlers->data == NULL)
		return -EINVAL;

	data = (struct bmp180_data *) handlers->data;

	rc = write_cmd("/sys/devices/virtual/input/input6/pressure_poll_delay", "66667000", 9);
	if (rc < 0) {
		ALOGE("%s: Unable to write sysfs value", __func__);
		return -1;
	}

	return 0;
}

float bmp180_convert(int value)
{
	return value / 100.0f;
}

extern int mFlushed;

int bmp180_get_data(struct smdk4x12_sensors_handlers *handlers,
	struct sensors_event_t *event)
{
	struct input_event input_event;
	int input_fd;
	int rc;
	int sensorId = SENSOR_TYPE_PRESSURE;

//	ALOGD("%s(%p, %p)", __func__, handlers, event);

	if (handlers == NULL || event == NULL)
		return -EINVAL;

	if (mFlushed & (1 << sensorId)) { /* Send flush META_DATA_FLUSH_COMPLETE immediately */
		sensors_event_t sensor_event;
		memset(&sensor_event, 0, sizeof(sensor_event));
		sensor_event.version = META_DATA_VERSION;
		sensor_event.type = SENSOR_TYPE_META_DATA;
		sensor_event.meta_data.sensor = sensorId;
		sensor_event.meta_data.what = 0;
		*event++ = sensor_event;
		mFlushed &= ~(0x01 << sensorId);
		ALOGD("AkmSensor: %s Flushed sensorId: %d", __func__, sensorId);
	}

	input_fd = handlers->poll_fd;
	if (input_fd < 0)
		return -EINVAL;

	memset(event, 0, sizeof(struct sensors_event_t));
	event->version = sizeof(struct sensors_event_t);
	event->sensor = handlers->handle;
	event->type = handlers->handle;

	do {
		rc = read(input_fd, &input_event, sizeof(input_event));
		if (rc < (int) sizeof(input_event))
			break;

		if (input_event.type == EV_REL) {
			switch (input_event.code) {
				case REL_HWHEEL:
					event->pressure = bmp180_convert(input_event.value);
					break;
				default:
					continue;
			}
		} else if (input_event.type == EV_SYN) {
			if (input_event.code == SYN_REPORT && event->pressure != 0) {
				event->timestamp = input_timestamp(&input_event);
				break;
			} else {
				return -1;
			}
		}
	} while (1);

	return 0;
}

struct smdk4x12_sensors_handlers bmp180 = {
	.name = "BMP180",
	.handle = SENSOR_TYPE_PRESSURE,
	.init = bmp180_init,
	.deinit = bmp180_deinit,
	.activate = bmp180_activate,
	.deactivate = bmp180_deactivate,
	.set_delay = bmp180_set_delay,
	.get_data = bmp180_get_data,
	.activated = 0,
	.needed = 0,
	.poll_fd = -1,
	.data = NULL,
};
