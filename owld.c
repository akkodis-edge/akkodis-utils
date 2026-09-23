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
	printf("  --delay         Time in seconds to buffer readings before writing to disk\n");
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
	int interval_ms;
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
	CYAML_FIELD_INT("interval_ms", CYAML_FLAG_DEFAULT, struct sensor_config, interval_ms),
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

struct owl_device {
	void (*free)(void*);
	void* priv;
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

static void owl_iio_free(void* priv)
{
	struct owl_iio_device *data = (struct owl_iio_device*) priv;
	if (data->chan != NULL)
		iio_channel_disable(data->chan);
	free(data);
}

static const struct libowl_sensor_ops iio_sensor_ops = {
	.read = iio_read,
};

static int create_owl_iio_device(struct iio_context* ctx, struct sensor_config* scfg, struct owl_device* dev, struct libowl_sensor_ops** ops)
{
	if (scfg->channel == NULL) {
		fprintf(stderr, "iio device \"%s\" missing mandatory \"channel\"\n", scfg->device);
		return -EINVAL;
	}
	if (scfg->attribute == NULL) {
		fprintf(stderr, "iio device \"%s\" missing mandatory \"attribute\"\n", scfg->device);
		return -EINVAL;
	}

	dev->free = owl_iio_free;
	dev->priv = calloc(1, sizeof(struct owl_iio_device));
	if (dev->priv == NULL)
		return -ENOMEM;
	struct owl_iio_device *data = (struct owl_iio_device*) dev->priv;

	data->dev = iio_context_find_device(ctx, scfg->device);
	if (data->dev == NULL) {
		fprintf(stderr, "iio device %s not found\n", scfg->device);
		return -ENOENT;
	}
	data->chan = iio_device_find_channel(data->dev, scfg->channel, 0);
	if (data->chan == NULL) {
		fprintf(stderr, "iio device %s channel %s not found\n", scfg->device, scfg->channel);
		return -ENOENT;
	}
	data->attr = (char*) scfg->attribute;
	*ops = (struct libowl_sensor_ops*) &iio_sensor_ops;
	return 0;
}

struct owl_file_device {
	char *path;
};

static int owl_file_read(int* value, void* priv)
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

static void owl_file_free(void* priv)
{
	struct owl_file_device *data = (struct owl_file_device*) priv;
	free(data);
}

static const struct libowl_sensor_ops file_sensor_ops = {
	.read = owl_file_read,
};

static int create_owl_file_device(struct sensor_config* scfg, struct owl_device* dev, struct libowl_sensor_ops** ops)
{
	dev->free = owl_file_free;
	dev->priv = calloc(1, sizeof(struct owl_file_device));
	if (dev->priv == NULL)
		return -ENOMEM;
	struct owl_file_device *data = (struct owl_file_device*) dev->priv;
	data->path = (char*) scfg->device;
	*ops = (struct libowl_sensor_ops*) &file_sensor_ops;
	return 0;
}

static void free_devices(struct owl_device* devices, size_t size)
{
	if (devices != NULL) {
		for (size_t i = 0; i < size; ++i) {
			if (devices[i].free != NULL && devices[i].priv != NULL)
				devices[i].free(devices[i].priv);
		}
		free(devices);
	}
}

static int create_devices(struct owl_device** devices, size_t* size, struct libowl* owl, const struct config* config, struct iio_context* ctx)
{
	struct owl_device *ptr = NULL;
	size_t ptr_size = 0;
	int r = 0;

	for (size_t i = 0; i < config->sensors_count; ++i) {
		printf("SENSOR: %s type: %d method: %d device: %s channel: %s attribute: %s\n",
			config->sensors[i].name, config->sensors[i].type, config->sensors[i].method,
			config->sensors[i].device, config->sensors[i].channel, config->sensors[i].attribute);
		/* allocate memory for device */
		struct owl_device *tmpptr = realloc(ptr, sizeof(*ptr) * (ptr_size + 1));
		if (tmpptr == NULL) {
			r = -ENOMEM;
			goto exit;
		}
		ptr = tmpptr;
		ptr_size++;

		const struct libowl_sensor_ops *ops = NULL;
		switch (config->sensors[i].method) {
		case OWLD_IIO:
			r = create_owl_iio_device(ctx, &config->sensors[i], &ptr[ptr_size - 1], (struct libowl_sensor_ops**) &ops);
			break;
		case OWLD_FILE:
			r = create_owl_file_device(&config->sensors[i], &ptr[ptr_size - 1], (struct libowl_sensor_ops**) &ops);
			break;
		default:
			r = -EINVAL;
			break;
		}
		if (r != 0) {
			fprintf(stderr, "sensor %s creating device [%d]: %s\n", config->sensors[i].name, -r, strerror(-r));
			goto exit;
		}
		r = libowl_add_sensor(owl, config->sensors[i].type, config->sensors[i].name, 0, ops, config->sensors[i].interval_ms, ptr[ptr_size - 1].priv);
		if (r != 0) {
			fprintf(stderr, "sensor %s failed adding to libowl [%d]: %s\n", config->sensors[i].name, -r, strerror(-r));
			goto exit;
		}
	}

	*devices = ptr;
	*size = ptr_size;
	r = 0;
exit:
	if (r != 0)
		free_devices(ptr, ptr_size);
	return r;
}

//NOLINTNEXTLINE(readability-function-cognitive-complexity)
int main (int argc, char **argv)
{
	char *database_path = NULL;
	char *config_path = NULL;
	int debug = 0;
	int delay_ms = 0;

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
		if (!strcmp("--delay", argv[i])) {
			if (++i >= argc) {
				fprintf(stderr, "Invalid --delay\n");
				return EINVAL;
			}
			char *endptr = NULL;
			const long result = strtol(argv[i], &endptr, 0);
			if (endptr == NULL || result < INT_MIN || result > (INT_MAX / 1000)) {
				fprintf(stderr, "Invalid --delay\n");
				return EINVAL;
			}
			delay_ms = (int) result * 1000;
		}
		else
		if (!strcmp("--help", argv[i]) || !strcmp("-h", argv[i])) {
			print_usage();
			return EINVAL;
		}
		else
		if (!strcmp("--debug", argv[i])) {
			debug = 1;
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
	struct owl_device *devices = NULL;
	size_t devices_count = 0;

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

	/* Parse config */
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

	/* open database */
	r = libowl_open(&owl, database_path, LIBOWL_OPEN_WRITE);
	if (r != 0) {
		r = -r;
		fprintf(stderr, "libowl: failed opening database file [%d]: %s\n", r, strerror(r));
		goto exit;
	}
	/* Set loglevel to output errors */
	libowl_set_loglevel(owl, debug ? LIBOWL_LOGLEVEL_DEBUG : LIBOWL_LOGLEVEL_ERROR);
	/* commit to disk every 10 seconds */
	libowl_set_buffer_duration(owl, delay_ms);

	/* Open iio context */
	ctx = iio_create_default_context();
	if (ctx == NULL) {
		fprintf(stderr, "Filed iio_create_default_context [%d]: %s\n", errno, strerror(errno));
		r = errno;
		goto exit;
	}

	/* Parse config for sensors */
	r = create_devices(&devices, &devices_count, owl, config, ctx);
	if (r != 0) {
		r = -r;
		goto exit;
	}

	while (true) {
		/* check for pending signals */
		const int delay = libowl_update_delay(owl);
		r = poll(&fds, 1, delay);
		if (r < 0) {
			r = errno;
			fprintf(stderr, "Failed polling [%d]: %s\n", r, strerror(r));
			goto exit;
		}

		/* prepare exit due to signal, set buffer duration 0 to ensure write to disk */
		if (fds.revents != 0)
			libowl_set_buffer_duration(owl, 0);

		/* update sensors */
		r = libowl_update(owl);
		if (r < 0) {
			fprintf(stderr, "failed polling sensors [%d]: %s\n", -r, strerror(-r));
			r = -r;
			goto exit;
		}

		/* exit due to signal */
		if (fds.revents != 0) {
			printf("INTERRUPT -- EXIT\n");
			break;
		}
	}

	r = 0;
exit:
	cyaml_free(&parser_conf, &config_schema, config, 0);
	if (ctx)
		iio_context_destroy(ctx);
	if (devices != NULL)
		free_devices(devices, devices_count);
	if (owl != NULL)
		libowl_close(owl);
	if (fds.fd >= 0)
		close(fds.fd);
	return r;
}
