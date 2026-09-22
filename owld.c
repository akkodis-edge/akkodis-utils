#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <cyaml/cyaml.h>
#include <iio.h>
#include "libowl.h"

static void print_usage()
{
	printf("owld, sensor logging utility, Akkodis Edge Sweden AB\n");
	printf("Mandatory:\n");
	printf("  -d/--database:  Database file\n");
	printf("  -c/--config:    Config file\n");
	printf("Optional\n");
	printf("  -h/--help:      This message\n");
	printf("Returns 0 if OK");
	printf("\n");
}

enum method {
	OWLD_IIO,
	OWLD_FILE,
};

struct sensor_config {
	const char *name;
	int type;
	int method;
	const char *device;
	const char *channel;
	const char *attribute;
};

struct config {
	struct sensor_config *sensors;
	size_t sensors_count;
};

static const cyaml_strval_t sensor_config_type_strings[] = {
	{"temperature", LIBOWL_SENSOR_TEMP},
};

static const cyaml_strval_t sensor_config_methods_strings[] = {
	{"iio", OWLD_IIO},
	{"file", OWLD_FILE},
};

static const cyaml_schema_field_t sensor_config_fields_schema[] = {
	CYAML_FIELD_STRING_PTR("name", CYAML_FLAG_POINTER, struct sensor_config, name,
			0, CYAML_UNLIMITED),
	CYAML_FIELD_ENUM("type", CYAML_FLAG_DEFAULT, struct sensor_config, type,
			sensor_config_type_strings, CYAML_ARRAY_LEN(sensor_config_type_strings)),
	CYAML_FIELD_ENUM("method", CYAML_FLAG_DEFAULT, struct sensor_config, method,
			sensor_config_methods_strings, CYAML_ARRAY_LEN(sensor_config_methods_strings)),
	CYAML_FIELD_STRING_PTR("device", CYAML_FLAG_POINTER, struct sensor_config, device,
			0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("channel", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, struct sensor_config, channel,
			0, CYAML_UNLIMITED),
	CYAML_FIELD_STRING_PTR("attribute", CYAML_FLAG_POINTER | CYAML_FLAG_OPTIONAL, struct sensor_config, attribute,
			0, CYAML_UNLIMITED),
	CYAML_FIELD_END
};

static const cyaml_schema_value_t sensor_config_schema = {
	CYAML_VALUE_MAPPING(CYAML_FLAG_DEFAULT, struct sensor_config, sensor_config_fields_schema),
};

static const cyaml_schema_field_t config_fields[] = {
	CYAML_FIELD_SEQUENCE("sensors", CYAML_FLAG_POINTER, struct config, sensors,
			&sensor_config_schema, 0, CYAML_UNLIMITED),
	CYAML_FIELD_END
};

static const cyaml_schema_value_t config_schema = {
	CYAML_VALUE_MAPPING(CYAML_FLAG_POINTER, struct config, config_fields),
};

struct owl_iio_device {
	struct iio_device *dev;
	struct iio_channel *chan;
	char *attr;
};

static int iio_read(int* value, void* priv)
{
	struct owl_iio_device *data = (struct owl_iio_device*) priv;

	long long attr = 0;
	int r = iio_channel_attr_read_longlong(data->chan, data->attr, &attr);
	if (r) {
		fprintf(stderr, "iio %s:%s failed reading attibute %s [%d]: %s\n",
				iio_device_get_name(data->dev), iio_channel_get_name(data->chan), data->attr,
				-r, strerror(-r));
		return r;
	}
	if (attr > INT_MAX || attr < INT_MIN)
		return -ERANGE;
	*value = (int) attr;
	return 0;
}

static const struct libowl_sensor_ops iio_sensor_ops = {
	.read = iio_read,
};

struct owl_file_device {
	char *path;
};

static int file_read(int* value, void* priv)
{
	struct owl_file_device *data = (struct owl_file_device*) priv;

	int fd = open(data->path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "file %s failed open [%d] %s\n", data->path, errno, strerror(errno));
		return -errno;
	}

	const size_t read_max = 64;
	uint8_t buf[read_max];
	buf[read_max - 1] = '\0';
	ssize_t bytes = read(fd, buf, read_max - 1);
	const int readerrno = errno;
	if (close(fd) != 0) {
		fprintf(stderr, "file %s failed closing [%d] %s\n", data->path, errno, strerror(errno));
		return -errno;
	}

	if (bytes < 0) {
		fprintf(stderr, "file %s failed reading [%d] %s\n", data->path, readerrno, strerror(readerrno));
		return -readerrno;
	}
	if (bytes < 1) {
		fprintf(stderr, "file %s read returned 0 bytes\n", data->path);
		return -EIO;
	}

	char *endptr = NULL;
	const long result = strtol((char*) buf, &endptr, 0);
	if (endptr == NULL || result < INT_MIN || result > INT_MAX) {
		fprintf(stderr, "file %s read returned invalid value\n", data->path);
		return -EFAULT;
	}
	*value = (int) result;

	return 0;
}

static const struct libowl_sensor_ops file_sensor_ops = {
	.read = file_read,
};

//NOLINTNEXTLINE(readability-function-cognitive-complexity)
int main (int argc, char **argv)
{
	char *database_path = NULL;
	char *config_path = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp("--database", argv[i]) || !strcmp("-d", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "Invalid -d/--database\n");
				return EINVAL;
			}
			database_path = argv[i];
		}
		else
		if (!strcmp("--config", argv[i]) || !strcmp("-c", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "Invalid -c/--config\n");
				return EINVAL;
			}
			config_path = argv[i];
		}
		else
		if (!strcmp("--help", argv[i]) || !strcmp("-h", argv[i])) {
			print_usage();
			return EINVAL;
		}
		else {
			fprintf(stderr, "Invalid argument\n");
			return EINVAL;
		}
	}

	if (!database_path) {
		fprintf(stderr, "Missing mandatory argument -d/--database\n");
		return EINVAL;
	}
	if (!config_path) {
		fprintf(stderr, "Missing mandatory argument -c/--config\n");
		return EINVAL;
	}

	struct libowl *owl = NULL;
	struct iio_context *ctx = NULL;
	struct config *config = NULL;
	struct owl_iio_device *iio_devices = NULL;
	size_t iio_devices_count = 0;
	struct owl_file_device *file_devices = NULL;
	size_t file_devices_count = 0;

	/* Install signal handler */
	struct pollfd fds;
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
		fprintf(stderr, "Failed blocking signals [%d]: %s\n", errno, strerror(errno));
		return errno;
	}
	fds.fd = signalfd(-1, &mask, 0);
	if (fds.fd < 0) {
		fprintf(stderr, "Failed installing signal handler [%d]: %s\n", errno, strerror(errno));
		return errno;
	}
	fds.events = POLLIN;

	cyaml_config_t parser_conf = {
		.log_fn = cyaml_log,
		.mem_fn = cyaml_mem,
		.log_level = CYAML_LOG_WARNING,
	};

	int r = cyaml_load_file(config_path, &parser_conf, &config_schema, (void**) &config, NULL);
	if (r != CYAML_OK) {
		fprintf(stderr, "Failed parsing configuration file\n");
		r = EINVAL;
		goto exit;
	}

	r = libowl_open(&owl, database_path, LIBOWL_OPEN_WRITE | LIBOWL_LOGLEVEL_DEBUG);
	if (r != 0) {
		r = -r;
		fprintf(stderr, "libowl: failed opening database file [%d]: %s\n", r, strerror(r));
		goto exit;
	}

	ctx = iio_create_default_context();
	if (ctx == NULL) {
		fprintf(stderr, "Filed iio_create_default_context [%d]: %s\n", errno, strerror(errno));
		r = errno;
		goto exit;
	}

	for (size_t i = 0; i < config->sensors_count; ++i) {
		printf("SENSOR: %s type: %d method: %d device: %s channel: %s attribute: %s\n",
			config->sensors[i].name, config->sensors[i].type, config->sensors[i].method,
			config->sensors[i].device, config->sensors[i].channel, config->sensors[i].attribute);
		switch (config->sensors[i].method) {
			case OWLD_IIO:
			{
				if (config->sensors[i].channel == NULL) {
					fprintf(stderr, "iio device \"%s\" missing mandatory \"channel\"\n", config->sensors[i].device);
					r = EINVAL;
					goto exit;
				}
				if (config->sensors[i].attribute == NULL) {
					fprintf(stderr, "iio device \"%s\" missing mandatory \"attribute\"\n", config->sensors[i].device);
					r = EINVAL;
					goto exit;
				}
				struct owl_iio_device *ptr = realloc(iio_devices, sizeof(*iio_devices) * (iio_devices_count + 1));
				if (ptr == NULL) {
					r = ENOMEM;
					goto exit;
				}
				iio_devices = ptr;
				iio_devices_count++;
				iio_devices[iio_devices_count - 1].dev = iio_context_find_device(ctx, config->sensors[i].device);
				if (iio_devices[iio_devices_count - 1].dev == NULL) {
					fprintf(stderr, "iio device %s not found\n", config->sensors[i].device);
					r = ENOENT;
					goto exit;
				}

				iio_devices[iio_devices_count - 1].chan = iio_device_find_channel(
						iio_devices[iio_devices_count - 1].dev, config->sensors[i].channel, 0);
				if (iio_devices[iio_devices_count - 1].chan == NULL) {
					fprintf(stderr, "iio device %s channel %s not found\n", config->sensors[i].device, config->sensors[i].channel);
					r = ENOENT;
					goto exit;
				}
				iio_devices[iio_devices_count - 1].attr = (char*) config->sensors[i].attribute;

				r = libowl_add_sensor(owl, config->sensors[i].type, config->sensors[i].name, 0, &iio_sensor_ops, 1000, &iio_devices[iio_devices_count - 1]);
				if (r != 0) {
					fprintf(stderr, "sensor %s failed adding to libowl [%d]: %s\n", config->sensors[i].name, -r, strerror(-r));
					r = -r;
					goto exit;
				}

				break;
			}
			case OWLD_FILE:
			{
				struct owl_file_device *ptr = realloc(file_devices, sizeof(*file_devices) * (file_devices_count + 1));
				if (ptr == NULL) {
					r = ENOMEM;
					goto exit;
				}
				file_devices = ptr;
				file_devices[file_devices_count].path = (char*) config->sensors[i].device;
				file_devices_count++;

				r = libowl_add_sensor(owl, config->sensors[i].type, config->sensors[i].name, 0, &file_sensor_ops, 1000, &file_devices[file_devices_count - 1]);
				if (r != 0) {
					fprintf(stderr, "sensor %s failed adding to libowl [%d]: %s\n", config->sensors[i].name, -r, strerror(-r));
					r = -r;
					goto exit;
				}
				break;
			}
			default:
				fprintf(stderr, "Invalid sensor %s of type %d\n", config->sensors[i].name, config->sensors[i].type);
				r = EINVAL;
				goto exit;
		}
	}

	/* last read sensor index */
	int64_t index = 0;

	while (true) {
		/* check for pending signals */
		const int delay = libowl_update_delay(owl);
		printf("delay: %d\n", delay);
		r = poll(&fds, 1, delay);
		if (r < 0) {
			r = errno;
			fprintf(stderr, "Failed polling [%d]: %s\n", r, strerror(r));
			goto exit;
		}

		/* Exit due to signal */
		if (fds.revents != 0) {
			printf("INTERRUPT -- EXIT\n");
			break;
		}

		/* update sensors */
		const int update_count = libowl_update(owl);
		if (update_count < 0) {
			fprintf(stderr, "failed polling sensors [%d]: %s\n", -update_count, strerror(-update_count));
			r = -update_count;
			goto exit;
		}
		printf("update: %d\n", update_count);
		/* Get values if any sensor updated */
		if (update_count > 0) {
			while (true) {
				size_t sensor_data_size = 50;
				struct libowl_sensor_data sensor_data[sensor_data_size];
				struct libowl_filter index_filter;
				if (libowl_filter_index(&index_filter, LIBOWL_OP_GREATER_EQUAL, index) != 0) {
					fprintf(stderr, "failed creating filter\n");
					r = EFAULT;
					goto exit;
				}

				r = libowl_read(owl, 0, &index_filter, 1, sensor_data, &sensor_data_size);
				if (r != 0) {
					fprintf(stderr, "failed reading sensors [%d]: %s\n", -update_count, strerror(-update_count));
					r = -update_count;
					goto exit;
				}

				if (sensor_data_size == 0)
					break;

				for (size_t i = 0; i < sensor_data_size; ++i) {
					char timestr[200];
					const time_t epoch = (time_t) sensor_data[i].epoch; /* double to time_t, drop fractional seconds */
					if (strftime(timestr, sizeof(timestr), "%Y-%m-%d %T", gmtime(&epoch)) < 1)
						timestr[0] = '\0';
					printf("[%s] %s: %d\n", timestr, sensor_data[i].name, sensor_data[i].value);
					if (sensor_data[i].index >= index)
						index = sensor_data[i].index + 1;
					libowl_sensor_data_free(&sensor_data[i]);
				}
			}
		}
	}

	r = 0;
exit:
	cyaml_free(&parser_conf, &config_schema, config, 0);
	if (ctx)
		iio_context_destroy(ctx);
	if (file_devices != NULL)
		free(file_devices);
	if (iio_devices != NULL) {
		for (size_t i = 0; i < iio_devices_count; ++i) {
			if (iio_devices[i].chan != NULL)
				iio_channel_disable(iio_devices[i].chan);
		}
		free(iio_devices);
	}
	if (owl != NULL)
		libowl_close(owl);
	if (fds.fd >= 0)
		close(fds.fd);
	return r;
}
