#ifndef LIBOWL__H__
#define LIBOWL__H__

#ifdef __cplusplus
extern "C" {
#endif

struct libowl;

/* create new connection to database at "path".
 *
 * "owl" must be closed by caller, see libowl_close()
 *
 * Returns 0 on success or negative errno for errors. */
enum libowl_open_flags {
	LIBOWL_OPEN_WRITE = 1 << 0,     /* Allow writing, in addition to reading */
	LIBOWL_LOGLEVEL_DEBUG = 1 << 1, /* Output debug messages */
};
int libowl_open(struct libowl** owl, const char* path, int flags);
int libowl_close(struct libowl* owl);

/*
 * Set monotonic clock implemenation. Allows replacing default monotonic clock
 * implementation with external function. */
int libowl_set_monotonic(struct libowl* owl, int (*monotonic)(struct timespec*, void*), void* monotonic_priv);

/* Types of sensors */
enum libowl_sensor_type {
	LIBOWL_SENSOR_TEMP, /* Temperature in milli C */
};
const char* libowl_sensor_type_str(int type);

/* Add sensor to libowl. Requires libowl opened with LIBOWL_OPEN_WRITE.
 * "interval_ms" defines polling interval.
 *
 * Returns 0 on success or negative errno for errors. */
enum libowl_sensor_flags {
	LIBOWL_SENSOR_FREE_PRIV = 1 << 0, /* Free priv data on call to libowl_close() */
};
struct libowl_sensor_ops {
	int (*read)(int* value, void* priv);
};
int libowl_add_sensor(struct libowl* owl, int type, const char* name, int flags, const struct libowl_sensor_ops* ops, int interval_ms, void* priv);

/* Read next sensor(s) and update to database. Requires libowl opened with LIBOWL_OPEN_WRITE.
 * Behaviour modified by timeout_ms:
 *     0: non-blocking
 *   < 0: blocking
 *   > 0: timeout
 * Returns 0 on no change, postive value for number of sensors updated or negative errno for errors. */
int libowl_next(struct libowl* owl, int timeout_ms);

/* Read from database into array
 * index is where to start reading from database
 *
 */
struct libowl_sensor_data {
	char *name; /* Points to libowl internal data, do not free */
	int64_t index;
	int type;
	int value;
};
int libowl_read(struct libowl* owl, int64_t index, struct libowl_sensor_data* data, size_t* size);

#ifdef __cplusplus
}
#endif

#endif // LIBOWL__H__
