#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <sqlite3.h>
#include "libowl.h"

struct libowl_sensor {
	char *name;
	int type;
	int flags;
	struct timespec last_poll;
	struct timespec interval;
	struct libowl_sensor_ops ops;
	void *priv;
};

struct libowl {
	struct sqlite3 *db;
	int flags;
	int loglevel;
	struct timespec buffer_duration;
	struct timespec buffer_end;
	struct libowl_sensor *sensors;
	size_t sensors_size;
	int (*monotonic)(struct timespec*, void*);
	void *monotonic_priv;
	struct libowl_sensor_data *buf;
	size_t buf_size;
	size_t buf_pos;
};

static void timespec_add(struct timespec* result, const struct timespec* lhs, const struct timespec* rhs)
{
	result->tv_sec = lhs->tv_sec + rhs->tv_sec;
	result->tv_nsec = lhs->tv_nsec + rhs->tv_nsec;
	while(result->tv_nsec >= 1000000000L) {
		result->tv_nsec -= 1000000000L;
		result->tv_sec++;
	}
}

static void timespec_substract(struct timespec* result, const struct timespec* lhs, const struct timespec* rhs)
{
	result->tv_sec = lhs->tv_sec - rhs->tv_sec;
	result->tv_nsec = lhs->tv_nsec - rhs->tv_nsec;
	while (result->tv_nsec < 0) {
		result->tv_nsec += 1000000000L;
		result->tv_sec--;
	}
}

/* return -1 if lhs < rhs, 0 if lhs == rhs and 1 if lhs > rhs*/
static int timespec_cmp(const struct timespec* lhs, const struct timespec* rhs)
{
	if (lhs->tv_sec == rhs->tv_sec && lhs->tv_nsec == rhs->tv_nsec)
		return 0;
	if (lhs->tv_sec > rhs->tv_sec
		|| (lhs->tv_sec == rhs->tv_sec && lhs->tv_nsec > rhs->tv_nsec))
		return 1;
	return -1;
}

static void timespec_from_ms(struct timespec* ts, int ms)
{
	ts->tv_sec = ms / 1000;
	ts->tv_nsec = (ms % 1000) * 1000000;
}

static int libowl_default_monotonic(struct timespec* time, void* priv)
{
	(void) priv;
	const int r = clock_gettime(CLOCK_MONOTONIC, time);
	if (r != 0)
		return -errno;
	return 0;
}

static int is_write(const struct libowl* owl)
{
	return (owl->flags & LIBOWL_OPEN_WRITE) == LIBOWL_OPEN_WRITE;
}

const char* libowl_sensor_type_str(int type)
{
	switch (type) {
	case LIBOWL_SENSOR_TEMP:
		return "TEMP";
	default:
		return NULL;
	}
}

static int libowl_sensor_type_int(const char* str)
{
	if (strcmp(str, "TEMP") == 0)
		return LIBOWL_SENSOR_TEMP;
	return INT_MAX;
}

static void mprint(FILE* stream, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vfprintf(stream, fmt, args);
	va_end(args);
}

#define pr_err(owl, fmt, ...) \
	if (owl->loglevel >= LIBOWL_LOGLEVEL_ERROR) \
		{mprint(stderr, "libowl: error: " fmt, ##__VA_ARGS__);}

#define pr_dbg(owl, fmt, ...) \
	if (owl->loglevel >= LIBOWL_LOGLEVEL_DEBUG) \
		{mprint(stderr, "libowl: dbg: " fmt, ##__VA_ARGS__);}

static int libowl_single_step(struct libowl* owl, const char* statement)
{
	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db, statement, -1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2(create_table) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = sqlite3_step(stmt);
	if (r != SQLITE_DONE) {
		pr_err(owl, "sqlite3_step(create_table) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = 0;
exit:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	return r;
}

static int libowl_populate_category(struct libowl* owl)
{
	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db,
		"INSERT OR IGNORE INTO category_type(name) VALUES (?)",
		-1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2(category) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	for (int i = 0; i <= LIBOWL_SENSOR_TEMP; ++i) {
		r = sqlite3_bind_text(stmt, 1, libowl_sensor_type_str(LIBOWL_SENSOR_TEMP), -1, SQLITE_STATIC);
		if (r != SQLITE_OK) {
			pr_err(owl, "sqlite3_bind_text(category) [%d]: %s\n", r, sqlite3_errstr(r));
			r = -EBADF;
			goto exit;
		}
		r = sqlite3_step(stmt);
		if (r != SQLITE_DONE) {
			pr_err(owl, "sqlite3_step(category) [%d]: %s\n", r, sqlite3_errstr(r));
			r = -EBADF;
			goto exit;
		}
	}

	r = 0;
exit:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	return r;
}

static int libowl_pragma(struct libowl* owl)
{
	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db,
		" PRAGMA journal_mode = DELETE;"
		" PRAGMA foreign_keys = ON;",
		-1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2(pragma) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	while (1) {
		r = sqlite3_step(stmt);
		if (r == SQLITE_DONE)
			break;
		if (r == SQLITE_ROW)
			continue;
		pr_err(owl, "sqlite3_step(pragma) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = 0;
exit:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	return r;
}

static int libowl_init_database(struct libowl* owl)
{
	int r = libowl_pragma(owl);
	if (r != 0)
		return r;

	r = libowl_single_step(owl,
		"CREATE TABLE IF NOT EXISTS category_type("
			"id INTEGER PRIMARY KEY,"
			"name TEXT NOT NULL,"
		   "UNIQUE(name)"
		") STRICT");
	if (r != 0)
		return r;

	r = libowl_populate_category(owl);
	if (r != 0)
		return r;

	r = libowl_single_step(owl,
		"CREATE TABLE IF NOT EXISTS sensors("
				"id INTEGER PRIMARY KEY,"
				"type_id INTEGER NOT NULL,"
				"name TEXT NOT NULL,"
				"FOREIGN KEY(type_id) REFERENCES category_type(id),"
				"UNIQUE(type_id, name)"
			") STRICT");
	if (r != 0)
		return r;

	r = libowl_single_step(owl,
		"CREATE TABLE IF NOT EXISTS data("
				"id INTEGER PRIMARY KEY,"
				"sensor_id INTEGER NOT NULL,"
				"value INTEGER NOT NULL,"
				"epoch REAL NOT NULL,"
				"FOREIGN KEY(sensor_id) REFERENCES sensors(id)"
			") STRICT");
	if (r != 0)
		return r;

	return 0;
}

int libowl_open(struct libowl** owl, const char* path, int flags)
{
	if (owl == NULL || *owl != NULL || path == NULL)
		return -EINVAL;
	struct libowl *newowl = calloc(1, sizeof(struct libowl));
	if (newowl == NULL)
		return -ENOMEM;

	newowl->monotonic = libowl_default_monotonic;
	newowl->flags = flags;
	newowl->loglevel = LIBOWL_LOGLEVEL_NONE;

	int sqlite3_flags = 0;
	if ((flags & LIBOWL_OPEN_WRITE) == LIBOWL_OPEN_WRITE)
		sqlite3_flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	else
		sqlite3_flags |= SQLITE_OPEN_READONLY;

	int r = sqlite3_open_v2(path, &newowl->db, sqlite3_flags, NULL);
	if (r != SQLITE_OK) {
		pr_err(newowl, "sqlite3_open_v2() [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	if ((flags & LIBOWL_OPEN_WRITE) == LIBOWL_OPEN_WRITE) {
		r = libowl_init_database(newowl);
		if (r != 0)
			goto exit;
	}

	/* Set busy handler */
	r = sqlite3_busy_timeout(newowl->db, 1000);
	if (r != SQLITE_OK) {
		pr_err(newowl, "sqlite3_busy_timeout() [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	*owl = newowl;
	newowl = NULL;
	r = 0;
exit:
	if (newowl != NULL)
		free(newowl);
	return r;
}

void libowl_set_loglevel(struct libowl* owl, int loglevel)
{
	if (owl == NULL)
		return;
	owl->loglevel = loglevel;
}

void libowl_set_buffer_duration(struct libowl* owl, int duration_ms)
{
	if (owl == NULL)
		return;
	timespec_from_ms(&owl->buffer_duration, duration_ms > 0 ? duration_ms : 0);
	/* disable any on-going buffer period if buffering is set to disabled */
	if (duration_ms < 1) {
		owl->buffer_end.tv_sec = 0;
		owl->buffer_end.tv_nsec = 0;
	}
}

int libowl_set_monotonic(struct libowl* owl, int (*monotonic)(struct timespec*, void*), void* monotonic_priv)
{
	if (owl == NULL || monotonic == NULL)
		return -EINVAL;
	owl->monotonic = monotonic;
	owl->monotonic_priv = monotonic_priv;
	return 0;
}

int libowl_close(struct libowl* owl)
{
	int r = 0;
	if (owl == NULL)
		return -EINVAL;
	if (owl->db != NULL) {
		r = sqlite3_close(owl->db);
		if (r != SQLITE_OK)
			return -EBUSY;
		owl->db = NULL;
	}
	if (owl->sensors != NULL) {
		for (size_t i = 0; i < owl->sensors_size; ++i) {
			if (owl->sensors[i].name != NULL)
				free(owl->sensors[i].name);
			if (owl->sensors[i].priv != NULL
					&& (owl->sensors[i].flags & LIBOWL_SENSOR_FREE_PRIV) == LIBOWL_SENSOR_FREE_PRIV)
				free(owl->sensors[i].priv);
		}
		free(owl->sensors);
		owl->sensors = NULL;
	}
	if (owl->buf != NULL)
		free(owl->buf);
	free(owl);
	return 0;
}

enum bind_type {
	BIND_IGNORE,
	BIND_TEXT,
	BIND_INT,
	BIND_INT64,
	BIND_DOUBLE,
};

struct libowl_bind {
	int col;
	enum bind_type type;
	union {
		const char *str;
		int integer;
		double dbl;
		int64_t i64;
	} data;
};

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static int libowl_stmt_bind(struct libowl* owl, struct sqlite3_stmt* stmt, const struct libowl_bind* bind, size_t size)
{
	int r = SQLITE_MISUSE;
	for (size_t i = 0; i < size; ++i) {
		switch (bind[i].type) {
		case BIND_TEXT:
			r = sqlite3_bind_text(stmt, bind[i].col, bind[i].data.str, -1, SQLITE_STATIC);
			if (r != SQLITE_OK) {
				pr_err(owl, "sqlite3_bind_text() [%d]: %s\n", r, sqlite3_errstr(r));
				goto exit;
			}
			break;
		case BIND_INT:
			r = sqlite3_bind_int(stmt, bind[i].col, bind[i].data.integer);
			if (r != SQLITE_OK) {
				pr_err(owl, "sqlite3_bind_int() [%d]: %s\n", r, sqlite3_errstr(r));
				goto exit;
			}
			break;
		case BIND_INT64:
			r = sqlite3_bind_int64(stmt, bind[i].col, bind[i].data.i64);
			if (r != SQLITE_OK) {
				pr_err(owl, "sqlite3_bind_int64() [%d]: %s\n", r, sqlite3_errstr(r));
				goto exit;
			}
			break;
		case BIND_DOUBLE:
			r = sqlite3_bind_double(stmt, bind[i].col, bind[i].data.dbl);
			if (r != SQLITE_OK) {
				pr_err(owl, "sqlite3_bind_double() [%d]: %s\n", r, sqlite3_errstr(r));
				goto exit;
			}
			break;
		case BIND_IGNORE:
			break;
		default:
			pr_err(owl, "Invalid sqlite3 bind: %d\n");
			r = SQLITE_ERROR;
			goto exit;
		}
	}
exit:
	return r == SQLITE_OK ? 0 : -EBADF;
}

static sqlite3_stmt* libowl_prepare_bind(struct libowl* owl, const char* sql, const struct libowl_bind* bind, size_t size)
{
	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db, sql, -1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2() [%d]: %s\n", r, sqlite3_errstr(r));
		goto exit;
	}

	r = libowl_stmt_bind(owl, stmt, bind, size);
	if (r != 0) {
		r = SQLITE_ERROR;
		goto exit;
	}

	r = SQLITE_OK;
exit:
	if (r != SQLITE_OK) {
		sqlite3_finalize(stmt);
		stmt = NULL;
	}
	return stmt;
}

int libowl_add_sensor(struct libowl* owl, int type, const char* name, int flags, const struct libowl_sensor_ops* ops, int interval_ms, void* priv)
{
	if (owl == NULL || !is_write(owl) || libowl_sensor_type_str(type) == NULL || name == NULL || name[0] == '\0' || ops == NULL || interval_ms < 0)
		return -EINVAL;

	void *ptr = realloc(owl->sensors, sizeof(*owl->sensors) * (owl->sensors_size + 1));
	if (ptr == NULL)
		return -ENOMEM;
	owl->sensors = ptr;
	struct libowl_sensor *sensor = &owl->sensors[owl->sensors_size];
	sensor->name = strdup(name);
	if (sensor->name == NULL)
		return -ENOMEM;
	owl->sensors_size++;
	sensor->type = type;
	sensor->flags = flags;
	timespec_from_ms(&sensor->interval, interval_ms);
	int r = owl->monotonic(&sensor->last_poll, owl->monotonic_priv);
	if (r != 0) {
		pr_err(owl, "owl->monotonic() [%d]: %s\n", r, strerror(r));
		return r;
	}
	memcpy(&sensor->ops, ops, sizeof(sensor->ops));
	sensor->priv = priv;

	const struct libowl_bind bind[] = {
		{.col = 1, .type = BIND_TEXT, .data.str = sensor->name},
		{.col = 2, .type = BIND_TEXT, .data.str = libowl_sensor_type_str(sensor->type)},
	};

	sqlite3_stmt *stmt = libowl_prepare_bind(owl,
		"INSERT OR IGNORE INTO sensors(name, type_id) VALUES "
			"(?, (SELECT id from category_type WHERE name=(?)))",
		bind, ARRAY_SIZE(bind));
	if (stmt == NULL)
		return -EBADF;

	r = sqlite3_step(stmt);
	if (r != SQLITE_DONE) {
		pr_err(owl, "sqlite3_step(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = 0;
exit:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	return r;
}

enum statement_option {
	STATEMENT_OPTION_FREE = 1 << 0, /* set if str should be freed */
};
struct libowl_statement_part {
	const char* str;
	int options;
};

static char* join_statement(const struct libowl_statement_part* parts, size_t size)
{
	/* Calculate required buffer-size */
	size_t len = 1; /* final null-terminator */
	for (size_t i = 0; i < size; ++i)
		len += strlen(parts[i].str);
	char *sql = malloc(len);
	if (sql == NULL)
		return NULL;
	size_t pos = 0;
	for (size_t i = 0; i < size; ++i) {
		const size_t tmplen = strlen(parts[i].str);
		memcpy(sql + pos, parts[i].str, tmplen);
		pos += tmplen;
	}
	sql[pos] = '\0';
	return sql;
}

static int libowl_sensor_push(struct libowl* owl, const struct libowl_sensor_data* data, size_t size, size_t* written)
{
	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db,
		"INSERT INTO data(sensor_id, value, epoch) VALUES "
			"((SELECT id from sensors WHERE type_id=(SELECT id from category_type WHERE name=(?)) AND name=(?)),"
			"?, (?))",
		-1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	*written = 0;

	for (size_t i = 0; i < size; ++i) {
		const struct libowl_bind bind[] = {
			{.col = 1, .type = BIND_TEXT, .data.str = libowl_sensor_type_str(data[i].type)},
			{.col = 2, .type = BIND_TEXT, .data.str = data[i].name},
			{.col = 3, .type = BIND_INT, .data.integer = data[i].value},
			{.col = 4, .type = BIND_DOUBLE, .data.dbl = data[i].epoch},
		};
		r = libowl_stmt_bind(owl, stmt, bind, ARRAY_SIZE(bind));
		if (r != 0)
			goto exit;
		r = sqlite3_step(stmt);
		if (r != SQLITE_DONE) {
			pr_err(owl, "sqlite3_step(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
			r = -EBADF;
			goto exit;
		}
		r = sqlite3_reset(stmt);
		if (r != SQLITE_OK) {
			pr_err(owl, "sqlite3_reset(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
			r = -EBADF;
			goto exit;
		}
		(*written)++;
	}
	r = 0;
exit:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	return r;
}

/* return 0 if not yet, 1 if now */
static int sensor_next_update(struct timespec* next_update, const struct timespec* time_now,
								const struct timespec* interval, const struct timespec* last_poll)
{
	timespec_add(next_update, interval, last_poll);
	return timespec_cmp(next_update, time_now) <= 0;
}

static void print_sensor_reading(const struct libowl* owl, const struct libowl_sensor_data* data)
{
	char timestr[200];
	const time_t seconds = (time_t) data->epoch; /* double to time_t, drop fractional seconds */
	if (strftime(timestr, sizeof(timestr), "%Y-%m-%d %T", gmtime(&seconds)) < 1)
		timestr[0] = '\0';
	pr_dbg(owl, "[%s] %s %s: %d\n", timestr, libowl_sensor_type_str(data->type), data->name, data->value);
}

static int libowl_get_epoch(struct libowl* owl, double* epoch)
{
	int r = 0;
	if ((owl->flags & LIBOWL_TIMESTAMP_MONOTONIC) == LIBOWL_TIMESTAMP_MONOTONIC) {
		struct timespec time_now;
		r = owl->monotonic(&time_now, owl->monotonic_priv);
		if (r != 0)
			return r;
		*epoch = (double) time_now.tv_sec + ((double) time_now.tv_nsec / 1.0e9);
		return 0;
	}

	sqlite3_stmt *stmt = NULL;
	r = sqlite3_prepare_v2(owl->db, "SELECT unixepoch('now', 'subsec')", -1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2(epoch) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
	r = sqlite3_step(stmt);
	if (r != SQLITE_ROW) {
		pr_err(owl, "sqlite3_step(epoch) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	*epoch = sqlite3_column_double(stmt, 0);
	r = 0;
exit:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	return r;
}

int libowl_update(struct libowl* owl)
{
	if (owl == NULL || !is_write(owl))
		return -EINVAL;

	struct timespec time_now;
	int r = owl->monotonic(&time_now, owl->monotonic_priv);
	if (r != 0)
		return r;

	for (size_t i = 0; i < owl->sensors_size; ++i) {
		/* Skip if sensor is not yet due for polling */
		struct timespec next_update;
		if (sensor_next_update(&next_update, &time_now, &owl->sensors[i].interval, &owl->sensors[i].last_poll) == 0)
			continue;
		/* read sensor */
		int value = 0;
		r = owl->sensors[i].ops.read(&value, owl->sensors[i].priv);
		memcpy(&owl->sensors[i].last_poll, &time_now, sizeof(owl->sensors[i].last_poll));
		if (r != 0) {
			pr_err(owl, "read sensor %s [%d]: %s\n", owl->sensors[i].name, r, strerror(r));
			continue;
		}
		/* prepare buffer space */
		if (owl->buf_pos <= owl->buf_size) {
			/* allocate space for 10 addition readings if no buffer available */
			struct libowl_sensor_data* ptr = (struct libowl_sensor_data*) realloc(owl->buf, sizeof(owl->buf[0]) * (owl->buf_size + 10));
			if (ptr == NULL)
				return -ENOMEM;
			owl->buf = ptr;
			owl->buf_size += 10;
		}
		/* fill in our sensor data */
		r = libowl_get_epoch(owl, &owl->buf[owl->buf_pos].epoch);
		if (r != 0)
			return r;
		owl->buf[owl->buf_pos].name = owl->sensors[i].name;
		owl->buf[owl->buf_pos].type = owl->sensors[i].type;
		owl->buf[owl->buf_pos].value = value;
		if (owl->loglevel >= LIBOWL_LOGLEVEL_DEBUG)
			print_sensor_reading(owl, &owl->buf[owl->buf_pos]);
		owl->buf_pos++;
	}

	/* Check whether to buffer data if available */
	if (owl->buf_pos > 0) {
		/* calculate buffer period unless already started */
		if (owl->buffer_end.tv_sec == 0 && owl->buffer_end.tv_nsec == 0)
			timespec_add(&owl->buffer_end, &time_now, &owl->buffer_duration);

		/* Check if time to write */
		if (timespec_cmp(&owl->buffer_end, &time_now) <= 0) {
			if (owl->buffer_duration.tv_sec > 0 || owl->buffer_duration.tv_nsec > 0)
				pr_dbg(owl, "write buffer\n");

			size_t written = 0;
			r = libowl_sensor_push(owl, owl->buf, owl->buf_pos, &written);
			/* move non written data to start of buffer */
			if (written > 0 && written < owl->buf_pos)
				memmove(owl->buf, &owl->buf[written], sizeof(owl->buf[0]) * (owl->buf_pos - written));
			owl->buf_pos -= written;
			/* reset buffer timer if all entries were written */
			if (owl->buf_pos == 0) {
				owl->buffer_end.tv_sec = 0;
				owl->buffer_end.tv_nsec = 0;
			}
			if (r < 0)
				return r;
			return written > INT_MAX ? INT_MAX : (int) written;
		}
	}

	return 0;
}

int libowl_update_delay(const struct libowl* owl)
{
	if (owl == NULL || !is_write(owl) || owl->sensors_size == 0)
		return 0;

	struct timespec time_now;
	int r = owl->monotonic(&time_now, owl->monotonic_priv);
	if (r != 0)
		return 0;

	struct timespec shortest = { .tv_sec = INT_MAX / 1000, .tv_nsec = 0};
	for (size_t i = 0; i < owl->sensors_size; ++i) {
		/* calculate timestamp for next update of sensor */
		struct timespec next_update;
		if (sensor_next_update(&next_update, &time_now, &owl->sensors[i].interval, &owl->sensors[i].last_poll) != 0)
			return 0; /* early exit if due for polling */
		/* Check if next update is shorter than previous shortest */
		struct timespec remaining;
		timespec_substract(&remaining, &next_update, &time_now);
		if (timespec_cmp(&remaining, &shortest) < 0)
			memcpy(&shortest, &remaining, sizeof(shortest));
	}

	/* timespec to milliseconds, check for overflow */
	if (shortest.tv_sec > (INT_MAX / 1000))
		return INT_MAX;
	int milliseconds = shortest.tv_sec * 1000;
	/* round-up nano to closes milli */
	const int nano_to_milli = (shortest.tv_nsec / 1000000)
								+ (shortest.tv_nsec % 1000000 ? 1 : 0);
	if ((INT_MAX - milliseconds) < nano_to_milli)
		return INT_MAX;
	return milliseconds + nano_to_milli;
}

int libowl_sensor_data_free(struct libowl_sensor_data* data)
{
	if (data != NULL) {
		if (data->name != NULL) {
			free((char*) data->name);
			data->name = NULL;
		}
	}
	return 0;
}

int libowl_filter_index(struct libowl_filter* filter, int op, int64_t index)
{
	if (op > LIBOWL_OP_EQUAL)
		return -EINVAL;
	filter->type = LIBOWL_FILTER_INDEX;
	filter->op = op;
	filter->data.mi64 = index;
	return 0;
}

int libowl_filter_epoch(struct libowl_filter* filter, int op, double epoch)
{
	if (op > LIBOWL_OP_EQUAL)
		return -EINVAL;
	filter->type = LIBOWL_FILTER_EPOCH;
	filter->op = op;
	filter->data.mdouble = epoch;
	return 0;
}

int libowl_filter_name(struct libowl_filter* filter, int op, const char* name)
{
	if (op > LIBOWL_OP_EQUAL || name == NULL)
		return -EINVAL;
	filter->type = LIBOWL_FILTER_NAME;
	filter->op = op;
	filter->data.str = name;
	return 0;
}

static const char* op_to_str(int op)
{
	switch (op) {
	case LIBOWL_OP_GREATER_THAN:
		return ">";
	case LIBOWL_OP_GREATER_EQUAL:
		return ">=";
	case LIBOWL_OP_LESS_THAN:
		return "<";
	case LIBOWL_OP_LESS_EQUAL:
		return "<=";
	case LIBOWL_OP_EQUAL:
		return "==";
	}
	return "XX";
}

static int filter_to_statement_and_bind(const struct libowl_filter* filter, size_t index, int column, struct libowl_statement_part* part, struct libowl_bind* bind)
{
	char *field = NULL;
	switch (filter->type) {
	case LIBOWL_FILTER_INDEX:
		bind->type = BIND_INT64;
		bind->data.i64 = filter->data.mi64;
		field = "A.id";
		break;
	case LIBOWL_FILTER_EPOCH:
		bind->type = BIND_DOUBLE;
		bind->data.dbl = filter->data.mdouble;
		field = "A.epoch";
		break;
	case LIBOWL_FILTER_NAME:
		bind->type = BIND_TEXT;
		bind->data.str = filter->data.str;
		field = "S.name";
		break;
	default:
		return -EINVAL;
	}
	bind->col = column;
	const int buf_size = 64;
	char buf[buf_size];
	const int bytes = snprintf(buf, buf_size, " %s%s %s (?)",
			index > 0 ? "AND " : "", field, op_to_str(filter->op));
	if (bytes < 0 || bytes >= buf_size)
		return -EINVAL;
	part->str = strdup(buf);
	if (part->str == NULL)
		return -ENOMEM;
	part->options |= STATEMENT_OPTION_FREE;
	return 0;
}

int libowl_read(struct libowl* owl, int flags, const struct libowl_filter* filters, size_t filter_size, struct libowl_sensor_data* data, size_t size)
{
	(void) flags;
	if (owl == NULL || filters == NULL || filter_size == 0 || data == NULL || size == 0 || size > INT_MAX)
		return -EINVAL;

	struct libowl_statement_part *parts = NULL;
	struct libowl_bind *bind = NULL;
	sqlite3_stmt *stmt = NULL;
	char *sql = NULL;
	size_t pos = 0;
	int r = 0;

	/* Allocate space for all required statement sections which will later be joined.
	 * base + filters + order + limit */
	const size_t part_size = 1 + filter_size + 1 + 1;
	parts = calloc(part_size ,sizeof(struct libowl_statement_part));
	if (parts == NULL) {
		r = -ENOMEM;
		goto exit;
	}

	/* Allocate space for all binding instructions to statement
	 * filters + limit */
	const size_t bind_size = filter_size + 1;
	bind = malloc(sizeof(struct libowl_bind) * bind_size);
	if (bind == NULL) {
		r = -ENOMEM;
		goto exit;
	}

	/* Base */
	parts[0].str =
		"SELECT "
			"A.id,"
			"(SELECT name from category_type WHERE id = S.type_id),"
			"S.name,"
			"A.value,"
			"A.epoch"
		" FROM data as A"
		" INNER JOIN sensors AS S on S.id = A.sensor_id"
		" WHERE";

	int bind_column = 1;
	/* filters  */
	for (size_t i = 0; i < filter_size; ++i) {
		r = filter_to_statement_and_bind(&filters[i], i, bind_column++, &parts[i + 1], &bind[i]);
		if (r != 0) {
			pr_err(owl, "invalid filter type: %d\n", filters[i].type);
			goto exit;
		}
	}

	/* Add order */
	const int is_descending = (flags & LIBOWL_READ_DESCENDING) == LIBOWL_READ_DESCENDING;
	parts[part_size - 2].str = is_descending ? " ORDER BY A.id DESC" : " ORDER BY A.id ASC";

	/* Add limit */
	parts[part_size - 1].str = " LIMIT (?)";
	bind[bind_size - 1].col = bind_column++;
	bind[bind_size - 1].type = BIND_INT;
	bind[bind_size - 1].data.integer = (int) size;

	/* Assemble statement */
	sql = join_statement(parts, part_size);
	if (sql == NULL) {
		r = -ENOMEM;
		goto exit;
	}

	/* compile statement and bind variables */
	stmt = libowl_prepare_bind(owl, sql, bind, bind_size);
	if (stmt == NULL) {
		r = -EBADF;
		goto exit;
	}

	/* retrieve data */
	do {
		r = sqlite3_step(stmt);
		switch (r) {
		case SQLITE_DONE:
			break;
		case SQLITE_ROW:
			data[pos].index = sqlite3_column_int64(stmt, 0);
			data[pos].type = libowl_sensor_type_int((const char*) sqlite3_column_text(stmt, 1));
			data[pos].name = strdup((const char*) sqlite3_column_text(stmt, 2));
			data[pos].value = sqlite3_column_int64(stmt, 3);
			data[pos].epoch = sqlite3_column_double(stmt, 4);
			pos++;
			break;
		default:
			pr_err(owl, "sqlite3_step(read) [%d]: %s\n", r, sqlite3_errstr(r));
			r = -EBADF;
			goto exit;
		}
	} while (r != SQLITE_DONE);

	r = pos;

exit:
	if (parts != NULL) {
		for (size_t i = 0; i < part_size; ++i) {
			if ((parts[i].options & STATEMENT_OPTION_FREE) == STATEMENT_OPTION_FREE)
				free((char*)parts[i].str);
		}
		free(parts);
	}
	if (bind != NULL)
		free(bind);
	if (sql != NULL)
		free(sql);
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	if (r < 0) {
		for (size_t i = 0; i < pos; ++i)
			libowl_sensor_data_free(&data[i]);
	}
	return r;
}
