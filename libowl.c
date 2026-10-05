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

enum internal_flags {
	INTERNAL_OPEN_WRITE          = 1 << 0,
	INTERNAL_TIMESTAMP_MONOTONIC = 1 << 1,
	INTERNAL_FULL                = 1 << 2,
	INTERNAL_ALLOW_TRIM          = 1 << 3,
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
	return (owl->flags & INTERNAL_OPEN_WRITE) == INTERNAL_OPEN_WRITE;
}

const char* libowl_sensor_type_str(int type)
{
	switch (type) {
	case LIBOWL_SENSOR_TEMP:
		return "TEMP";
	case LIBOWL_SENSOR_VOLTAGE:
		return "VOLTAGE";
	case LIBOWL_SENSOR_CURRENT:
		return "CURRENT";
	case LIBOWL_SENSOR_RATIO:
		return "RATIO";
	case LIBOWL_SENSOR_COUNTER:
		return "COUNTER";
	default:
		return NULL;
	}
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

static void bind_text(struct libowl_bind* bind, int col, const char* str)
{
	bind->col = col;
	bind->type = BIND_TEXT;
	bind->data.str = str;
}

static void bind_int(struct libowl_bind* bind, int col, int value)
{
	bind->col = col;
	bind->type = BIND_INT;
	bind->data.integer = value;
}

static void bind_int64(struct libowl_bind* bind, int col, int64_t value)
{
	bind->col = col;
	bind->type = BIND_INT64;
	bind->data.i64 = value;
}

static void bind_double(struct libowl_bind* bind, int col, double value)
{
	bind->col = col;
	bind->type = BIND_DOUBLE;
	bind->data.dbl = value;
}

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static int libowl_stmt_col(struct libowl* owl, struct sqlite3_stmt* stmt, struct libowl_bind* bind, size_t size)
{
	for (size_t i = 0; i < size; ++i) {
		switch (bind[i].type) {
		case BIND_TEXT:
			if (sqlite3_column_type(stmt, bind[i].col) != SQLITE_TEXT)
				return -EBADF;
			bind[i].data.str = (const char*) sqlite3_column_text(stmt, bind[i].col);
			if (bind[i].data.str == NULL && sqlite3_errcode(owl->db) != SQLITE_OK)
				return -EBADF;
			break;
		case BIND_INT:
			if (sqlite3_column_type(stmt, bind[i].col) != SQLITE_INTEGER)
				return -EBADF;
			bind[i].data.integer = sqlite3_column_int(stmt, bind[i].col);
			break;
		case BIND_INT64:
			if (sqlite3_column_type(stmt, bind[i].col) != SQLITE_INTEGER)
				return -EBADF;
			bind[i].data.i64 = sqlite3_column_int64(stmt, bind[i].col);
			break;
		case BIND_DOUBLE:
			if (sqlite3_column_type(stmt, bind[i].col) != SQLITE_FLOAT)
				return -EBADF;
			bind[i].data.dbl = sqlite3_column_double(stmt, bind[i].col);
			break;
		case BIND_IGNORE:
			break;
		default:
			pr_err(owl, "Invalid sqlite3 col: %d\n");
			return -EBADF;
		}
	}
	return 0;
}

static int libowl_stmt_bind(struct libowl* owl, struct sqlite3_stmt* stmt, const struct libowl_bind* bind, size_t size)
{
	int r = SQLITE_OK;
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

static sqlite3_stmt* libowl_single(struct libowl* owl, const char* sql, const struct libowl_bind* input, size_t input_size, struct libowl_bind* output, size_t output_size)
{
	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db, sql, -1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2() [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	if (input != NULL && input_size > 0) {
		r = libowl_stmt_bind(owl, stmt, input, input_size);
		if (r != 0)
			goto exit;
	}

	r = sqlite3_step(stmt);
	if (r != SQLITE_DONE && r != SQLITE_ROW) {
		pr_err(owl, "sqlite3_step() [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
	if (output != NULL && output_size > 0) {
		r = libowl_stmt_col(owl, stmt, output, output_size);
		if (r != 0) {
			pr_err(owl, "failed reading statement values\n");
			goto exit;
		}
	}

	r = 0;
exit:
	if (r != 0) {
		if (stmt != NULL)
			sqlite3_finalize(stmt);
		stmt = NULL;
	}
	return stmt;
}

static int libowl_single_simple(struct libowl* owl, const char* sql)
{
	sqlite3_stmt *stmt = libowl_single(owl, sql, NULL, 0, NULL, 0);
	sqlite3_finalize(stmt);
	return stmt == NULL ? -EBADF : 0;
}

static int libowl_init_database(struct libowl* owl)
{
	int r = libowl_single_simple(owl, "PRAGMA journal_mode = DELETE");
	if (r != 0)
		return r;
	r = libowl_single_simple(owl, "PRAGMA foreign_keys = ON");
	if (r != 0)
		return r;

	r = libowl_single_simple(owl,
		"CREATE TABLE IF NOT EXISTS sensors("
				"id INTEGER PRIMARY KEY,"
				"type_id INTEGER NOT NULL CHECK(type_id >= 0),"
				"name TEXT NOT NULL,"
				"UNIQUE(type_id, name)"
			") STRICT");
	if (r != 0)
		return r;

	r = libowl_single_simple(owl,
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

int libowl_open(struct libowl** owl, const char* path, int flags)
{
	if (owl == NULL || *owl != NULL || path == NULL)
		return -EINVAL;
	struct libowl *newowl = calloc(1, sizeof(struct libowl));
	if (newowl == NULL)
		return -ENOMEM;

	newowl->monotonic = libowl_default_monotonic;
	newowl->flags = 0;
	if ((flags & LIBOWL_OPEN_WRITE) == LIBOWL_OPEN_WRITE)
		newowl->flags |= INTERNAL_OPEN_WRITE;
	if ((flags & LIBOWL_TIMESTAMP_MONOTONIC) == LIBOWL_TIMESTAMP_MONOTONIC)
		newowl->flags |= INTERNAL_TIMESTAMP_MONOTONIC;
	if ((flags & LIBOWL_ALLOW_TRIM) == LIBOWL_ALLOW_TRIM)
		newowl->flags |= INTERNAL_ALLOW_TRIM;
	newowl->loglevel = LIBOWL_LOGLEVEL_NONE;

	int sqlite3_flags = 0;
	if ((flags & INTERNAL_OPEN_WRITE) == INTERNAL_OPEN_WRITE)
		sqlite3_flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	else
		sqlite3_flags |= SQLITE_OPEN_READONLY;

	int r = sqlite3_open_v2(path, &newowl->db, sqlite3_flags, NULL);
	if (r != SQLITE_OK) {
		pr_err(newowl, "sqlite3_open_v2() [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	if ((flags & INTERNAL_OPEN_WRITE) == INTERNAL_OPEN_WRITE) {
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
		libowl_close(newowl);
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

int64_t libowl_get_size_impl(struct libowl* owl, const char* page_count_statement)
{
	struct libowl_bind bind_int64 = {
		.col = 0, .type = BIND_INT64
	};

	sqlite3_stmt *stmt = libowl_single(owl, "PRAGMA page_size;", NULL, 0, &bind_int64, 1);
	if (stmt == NULL)
		return -EBADF;
	const int64_t page_size = bind_int64.data.i64;
	sqlite3_finalize(stmt);

	stmt = libowl_single(owl, page_count_statement, NULL, 0, &bind_int64, 1);
	if (stmt == NULL)
		return -EBADF;
	const int64_t page_count = bind_int64.data.i64;
	sqlite3_finalize(stmt);

	if (page_size < 1 || page_count < 0)
		return -EBADF;
	if ((INT64_MAX / page_size) < page_count)
		return -ERANGE;
	return page_size * page_count;
}

int64_t libowl_get_size(struct libowl* owl)
{
	return libowl_get_size_impl(owl, "PRAGMA page_count;");
}

int64_t libowl_get_maximum_size(struct libowl* owl)
{
	return libowl_get_size_impl(owl, "PRAGMA max_page_count;");
}

int64_t libowl_get_minimum_size(struct libowl* owl)
{
	return libowl_get_size_impl(owl, "SELECT 4");
}

int64_t libowl_set_maximum_size(struct libowl* owl, int64_t bytes)
{
	if (bytes < libowl_get_minimum_size(owl))
		return -EINVAL;
	if (bytes < libowl_get_size(owl))
		return -EFBIG;

	int r = 0;

	struct libowl_bind bind_int64 = {
		.col = 0, .type = BIND_INT64
	};
	sqlite3_stmt *stmt = libowl_single(owl, "PRAGMA page_size;", NULL, 0, &bind_int64, 1);
	if (stmt == NULL)
		return -EBADF;
	const int64_t page_size = bind_int64.data.i64;
	sqlite3_finalize(stmt);

	/* calculate number of pages */
	if (page_size < 1)
		return -EBADF;
	int64_t requested_max_page_count = bytes / page_size;
	/* round up bytes to full pages */
	if (bytes % page_size)
		requested_max_page_count++;

	/* Write value */
	const int buf_size = 128;
	char buf[buf_size];
	r = snprintf(buf, buf_size, "PRAGMA max_page_count=%" PRId64 ";", requested_max_page_count);
	if (r < 0 || r >= buf_size)
		return -EBADF;
	stmt = libowl_single(owl, buf, NULL, 0, &bind_int64, 1);
	if (stmt == NULL)
		return -EBADF;
	const int64_t max_page_count = bind_int64.data.i64;
	sqlite3_finalize(stmt);

	if (max_page_count < 1)
		return -EBADF;
	if ((INT64_MAX / page_size) < max_page_count)
		return -ERANGE;

	return max_page_count * page_size;
}

int libowl_add_sensor(struct libowl* owl, int type, const char* name, int flags, const struct libowl_sensor_ops* ops, int interval_ms, void* priv)
{
	if (owl == NULL || !is_write(owl) || libowl_sensor_type_str(type) == NULL || name == NULL || name[0] == '\0' || ops == NULL || interval_ms < 0)
		return -EINVAL;

	/* Block creation if we already have a sensor with same name and type */
	for (size_t i = 0; i < owl->sensors_size; ++i) {
		if (type == owl->sensors[i].type && strcmp(name, owl->sensors[i].name) == 0)
			return -EEXIST;
	}

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
		{.col = 2, .type = BIND_INT, .data.integer = sensor->type},
	};

	sqlite3_stmt *stmt = libowl_prepare_bind(owl,
		"INSERT OR IGNORE INTO sensors(name, type_id) VALUES (?, ?)",
		bind, ARRAY_SIZE(bind));
	if (stmt == NULL)
		return -EBADF;

	r = sqlite3_step(stmt);
	if (r != SQLITE_DONE) {
		pr_err(owl, "sqlite3_step(add_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
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

static int libowl_sensor_delete(struct libowl* owl, int64_t size)
{
	const struct libowl_bind bind_delete = {
		.col = 1, .type = BIND_INT64, .data.i64 = size
	};
	sqlite3_stmt *stmt = libowl_single(owl,
			"DELETE FROM data WHERE id IN (SELECT id FROM data LIMIT (?))",
			&bind_delete, 1, NULL, 0);
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	return stmt == NULL ? -EBADF : 0;
}

static int libowl_sensor_push(struct libowl* owl, const struct libowl_sensor_data* data, size_t size, size_t* written, int delete_before_insert)
{
	if (size > INT64_MAX)
		return -EINVAL;

	sqlite3_stmt *stmt = NULL;
	/* Explictly start write transaction to avoid autocommit for each sqlite3_step() call */
	int r = libowl_single_simple(owl, "BEGIN IMMEDIATE");
	if (r != 0)
		goto exit;

	if (delete_before_insert) {
		r = libowl_sensor_delete(owl, (int64_t) size);
		if (r != 0)
			goto exit;
	}

	r = sqlite3_prepare_v2(owl->db,
		"INSERT INTO data(sensor_id, value, epoch) VALUES "
			"((SELECT id from sensors WHERE type_id=(?) AND name=(?)), (?), (?))",
		-1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2(insert_data) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	*written = 0;
	size_t written_entries = 0;

	for (size_t i = 0; i < size; ++i) {
		const struct libowl_bind bind[] = {
			{.col = 1, .type = BIND_INT, .data.integer = data[i].type},
			{.col = 2, .type = BIND_TEXT, .data.str = data[i].name},
			{.col = 3, .type = BIND_INT, .data.integer = data[i].value},
			{.col = 4, .type = BIND_DOUBLE, .data.dbl = data[i].epoch},
		};
		r = libowl_stmt_bind(owl, stmt, bind, ARRAY_SIZE(bind));
		if (r != 0)
			goto exit;
		r = sqlite3_step(stmt);
		if (r != SQLITE_DONE) {
			pr_err(owl, "sqlite3_step(insert_data) [%d]: %s\n", r, sqlite3_errstr(r));
			switch (r) {
			case SQLITE_FULL:
				r = -EDQUOT;
				break;
			default:
				r = -EBADF;
				break;
			}
			goto exit;
		}
		r = sqlite3_reset(stmt);
		if (r != SQLITE_OK) {
			pr_err(owl, "sqlite3_reset(insert_data) [%d]: %s\n", r, sqlite3_errstr(r));
			r = -EBADF;
			goto exit;
		}
		written_entries++;
	}
	/* commit explicitly started transaction */
	r = libowl_single_simple(owl, "COMMIT");
	if (r != 0)
		goto exit;

	*written = written_entries;
	r = 0;
exit:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	/* Rollback if transcation on-going. On success should have been closed by COMMIT */
	if (sqlite3_txn_state(owl->db, NULL) >= SQLITE_TXN_WRITE) {
		const int res = libowl_single_simple(owl, "ROLLBACK");
		if (r == 0)
			r = res;
	}

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
	if ((owl->flags & INTERNAL_TIMESTAMP_MONOTONIC) == INTERNAL_TIMESTAMP_MONOTONIC) {
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

			size_t total_written = 0;
			const int is_trim = (owl->flags & INTERNAL_ALLOW_TRIM) == INTERNAL_ALLOW_TRIM;
			for (int i = 0; i < (is_trim ? 3 : 1); ++i) {
				const int is_delete_before_insert = (owl->flags & INTERNAL_FULL) == INTERNAL_FULL;
				size_t written = 0;
				r = libowl_sensor_push(owl, owl->buf, owl->buf_pos, &written, is_delete_before_insert);
				if (r == -EDQUOT && (owl->flags & INTERNAL_FULL) != INTERNAL_FULL) {
					owl->flags |= INTERNAL_FULL;
					if (is_trim)
						pr_dbg(owl, "database full, enabling delete-before-insert mode\n");
				}

				/* Write transactions will fail if there is not enough free space in
				 * database for all entries, space freed by "is_delete_before_insert"
				 * is not taken into account for the current transaction and buffer
				 * of free space is required. Create that here, if allowed. */
				if (r == -EDQUOT && is_trim) {
					/* Create a buffer of entries to be written + 50% (roundup)
					 * to account for any changes in INTEGER sizes. */
					size_t buffer_entries_extra = owl->buf_pos / 2 + (owl->buf_pos % 2 ? 1 : 0);
					const size_t buffer_entries = (SIZE_MAX - buffer_entries_extra) > owl->buf_pos
							? owl->buf_pos + buffer_entries_extra : SIZE_MAX;
					pr_dbg(owl, "database full, freeing transaction buffer of size: %zu\n", buffer_entries);
					r = libowl_sensor_delete(owl, buffer_entries);
					if (r == 0)
						break;
					continue;
				}
				if(r != 0)
					break;
				/* move non written data to start of buffer */
				if (written > 0 && written < owl->buf_pos)
					memmove(owl->buf, &owl->buf[written], sizeof(owl->buf[0]) * (owl->buf_pos - written));
				owl->buf_pos -= written;
				total_written += written;
				/* reset buffer timer if all entries were written and exit write loop */
				if (owl->buf_pos == 0) {
					owl->buffer_end.tv_sec = 0;
					owl->buffer_end.tv_nsec = 0;
					break;
				}
			}
			if (r != 0)
				return r;
			return total_written > INT_MAX ? INT_MAX : (int) total_written;
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

/* Used as array index, modify with care */
enum libowl_sensor_filter_type {
	LIBOWL_FILTER_EPOCH,
	LIBOWL_FILTER_INDEX,
	LIBOWL_FILTER_NAME,
	LIBOWL_FILTER_TYPE,
	LIBOWL_FILTER_ARRAY_SIZE,
};

int libowl_filter_index(struct libowl_filter* filter, int op, int64_t index)
{
	if (filter == NULL || op > LIBOWL_OP_IN)
		return -EINVAL;
	filter->type = LIBOWL_FILTER_INDEX;
	filter->op = op;
	filter->data.mi64 = index;
	return 0;
}

int libowl_filter_epoch(struct libowl_filter* filter, int op, double epoch)
{
	if (filter == NULL || op > LIBOWL_OP_IN)
		return -EINVAL;
	filter->type = LIBOWL_FILTER_EPOCH;
	filter->op = op;
	filter->data.mdouble = epoch;
	return 0;
}

int libowl_filter_name(struct libowl_filter* filter, int op, const char* name)
{
	if (filter == NULL || op > LIBOWL_OP_IN || name == NULL)
		return -EINVAL;
	filter->type = LIBOWL_FILTER_NAME;
	filter->op = op;
	filter->data.str = name;
	return 0;
}

int libowl_filter_type(struct libowl_filter* filter, int op, int type)
{
	if (filter == NULL || op > LIBOWL_OP_IN || libowl_sensor_type_str(type) == NULL)
		return -EINVAL;
	filter->type = LIBOWL_FILTER_TYPE;
	filter->op = op;
	filter->data.mint = type;
	return 0;
}

enum libowl_option_type {
	LIBOWL_OPTION_DESCENDING,
	LIBOWL_OPTION_INTERVAL,
	LIBOWL_OPTION_AVG,
	LIBOWL_OPTION_MIN,
	LIBOWL_OPTION_MAX,
};

int libowl_option_descending(struct libowl_option* option)
{
	if (option == NULL)
		return -EINVAL;
	option->type = LIBOWL_OPTION_DESCENDING;
	return 0;
}

int libowl_option_interval(struct libowl_option* option, double interval)
{
	if (option == NULL || interval < 0.001)
		return -EINVAL;
	const double milliseconds = interval * 1000;
	if (isinf(milliseconds))
		return -ERANGE;
	option->type = LIBOWL_OPTION_INTERVAL;
	option->data.mdouble = milliseconds;
	return 0;
}

int libowl_option_avg(struct libowl_option* option)
{
	if (option == NULL)
		return -EINVAL;
	option->type = LIBOWL_OPTION_AVG;
	return 0;
}

int libowl_option_min(struct libowl_option* option)
{
	if (option == NULL)
		return -EINVAL;
	option->type = LIBOWL_OPTION_MIN;
	return 0;
}

int libowl_option_max(struct libowl_option* option)
{
	if (option == NULL)
		return -EINVAL;
	option->type = LIBOWL_OPTION_MAX;
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
	case LIBOWL_OP_NOT_EQUAL:
		return "!=";
	case LIBOWL_OP_IN:
		return "IN";
	}
	return NULL;
}

static const char* filter_type_to_field(int type)
{
	switch (type) {
	case LIBOWL_FILTER_INDEX:
		return "A.id";
	case LIBOWL_FILTER_EPOCH:
		return "A.epoch";
	case LIBOWL_FILTER_NAME:
		return "S.name";
	case LIBOWL_FILTER_TYPE:
		return "S.type_id";
	}
	return NULL;
}

static void filter_bind(struct libowl_bind* bind, int column, const struct libowl_filter* filter)
{
	switch (filter->type) {
	case LIBOWL_FILTER_INDEX:
		bind_int64(bind, column, filter->data.mi64);
		break;
	case LIBOWL_FILTER_EPOCH:
		bind_double(bind, column, filter->data.mdouble);
		break;
	case LIBOWL_FILTER_NAME:
		bind_text(bind, column, filter->data.str);
		break;
	case LIBOWL_FILTER_TYPE:
		bind_int(bind, column, filter->data.mint);
		break;
	}
}

static char* allocate_filter_part(const char* prefix, const char* field, const char* op, const char* suffix)
{
	const int buf_size = 64;
	char buf[buf_size];
	const int bytes = snprintf(buf, buf_size, " %s%s %s %s",
			prefix, field, op, suffix);
	if (bytes < 0 || bytes >= buf_size)
		return NULL;
	return strdup(buf);
}

struct filters_data {
	size_t part_count; /* required space in parts buffer */
	size_t bind_count; /* required space in bind buffer */
};
/* if part or bind are NULL, then only returns filters_data and does not write to neither bind nor part */
static int filters_to_statement_and_bind(struct filters_data* retdata, const struct libowl_filter* filters, size_t filters_size, int start_column, struct libowl_statement_part* part, struct libowl_bind* bind)
{
	const int allow_write = bind != NULL && part != NULL;
	int add_and_prefix = 0;
	size_t part_count = 0;
	size_t bind_count = 0;
	size_t op_in_count_by_type[LIBOWL_FILTER_ARRAY_SIZE];
	memset(op_in_count_by_type, 0, sizeof(op_in_count_by_type));

	/* Handle all simple binds and count LIBOWL_OP_IN per type */
	for (size_t i = 0; i < filters_size; ++i) {
		/* invalid type */
		if (filter_type_to_field(filters[i].type) == NULL
				|| op_to_str(filters[i].op) == NULL)
			return -EINVAL;
		/* Only count in first pass if LIBOWL_OP_IN */
		if (filters[i].op == LIBOWL_OP_IN) {
			op_in_count_by_type[filters[i].type]++;
			continue;
		}
		/* Bind if simple */
		if (allow_write) {
			part[part_count].str = allocate_filter_part(add_and_prefix ? "AND " : "",
					filter_type_to_field(filters[i].type), op_to_str(filters[i].op), "(?)");
			if (part[part_count].str == NULL)
				return -ENOMEM;
			part[part_count].options |= STATEMENT_OPTION_FREE;
			filter_bind(&bind[bind_count], start_column + bind_count, &filters[i]);
		}
		part_count++;
		bind_count++;
		add_and_prefix = 1;
	}

	/* Handle LIBOWL_OP_IN */
	for (size_t j = 0; j < LIBOWL_FILTER_ARRAY_SIZE; ++j) {
		/* skip if no ops to handle */
		if (op_in_count_by_type[j] < 1)
			continue;

		/* add start parenthesis */
		if (allow_write) {
			part[part_count].str = allocate_filter_part(add_and_prefix ? "AND " : "",
					filter_type_to_field(j), op_to_str(LIBOWL_OP_IN), "(");
			if (part[part_count].str == NULL)
				return -ENOMEM;
			part[part_count].options |= STATEMENT_OPTION_FREE;
		}
		part_count++;

		/* Add values */
		int first_value = 1;
		for (size_t i = 0; i < filters_size; ++i) {
			/* skip wrong types or ops */
			if (filters[i].type != (int) j || filters[i].op != LIBOWL_OP_IN)
				continue;
			if (allow_write) {
				part[part_count].str = first_value ? "(?)" : ", (?)";
				filter_bind(&bind[bind_count], start_column + bind_count, &filters[i]);
			}
			part_count++;
			bind_count++;
			first_value = 0;
		}
		/* Add end parenthesis */
		if (allow_write) {
			part[part_count].str = ")";
		}
		part_count++;
		add_and_prefix = 1;
	}

	retdata->part_count = part_count;
	retdata->bind_count = bind_count;

	return 0;
}

enum read_options_flags {
	READ_OPTION_DESCENDING = 1 << 0,
	READ_OPTION_INTERVAL   = 1 << 1,
	READ_OPTION_AVG        = 1 << 2,
	READ_OPTION_MIN        = 1 << 3,
	READ_OPTION_MAX        = 1 << 4,
	READ_OPTION_AGGREGATE_MASK = (READ_OPTION_AVG | READ_OPTION_MIN | READ_OPTION_MAX),
};

struct read_options {
	int flags;
	double interval;
};

static int parse_options(struct read_options* ropts, const struct libowl_option* options, size_t size)
{
	if (options == NULL)
		return 0;

	for (size_t i = 0; i < size; ++i) {
		switch(options[i].type) {
		case LIBOWL_OPTION_DESCENDING:
			ropts->flags |= READ_OPTION_DESCENDING;
			break;
		case LIBOWL_OPTION_INTERVAL:
			ropts->flags |= READ_OPTION_INTERVAL;
			ropts->interval = options[i].data.mdouble;
			break;
		case LIBOWL_OPTION_AVG:
			ropts->flags |= READ_OPTION_AVG;
			break;
		case LIBOWL_OPTION_MIN:
			ropts->flags |= READ_OPTION_MIN;
			break;
		case LIBOWL_OPTION_MAX:
			ropts->flags |= READ_OPTION_MAX;
			break;
		default:
			return -EINVAL;
		}
	}

	/* Can't have more than 1 aggregate */
	int aggregate_count = 0;
	if ((ropts->flags & READ_OPTION_AVG) == READ_OPTION_AVG)
			aggregate_count++;
	if ((ropts->flags & READ_OPTION_MIN) == READ_OPTION_MIN)
			aggregate_count++;
	if ((ropts->flags & READ_OPTION_MAX) == READ_OPTION_MAX)
			aggregate_count++;
	if (aggregate_count > 1)
		return -EINVAL;

	/* Interval must have an aggregate */
	if ((ropts->flags & READ_OPTION_INTERVAL) == READ_OPTION_INTERVAL
			&& aggregate_count < 1)
		return -EINVAL;

	return 0;
}

int libowl_read(struct libowl* owl, const struct libowl_option* options, size_t option_size,
									const struct libowl_filter* filters, size_t filter_size,
									struct libowl_sensor_data* data, size_t size)
{
	if (owl == NULL || filters == NULL || filter_size == 0 || filter_size > INT_MAX || data == NULL || size == 0 || size > INT_MAX)
		return -EINVAL;

	struct libowl_statement_part *parts = NULL;
	struct libowl_bind *bind = NULL;
	sqlite3_stmt *stmt = NULL;
	char *sql = NULL;
	size_t pos = 0;
	int r = 0;

	/* parse options */
	struct read_options ropts;
	memset(&ropts, 0, sizeof(ropts));
	r = parse_options(&ropts, options, option_size);
	if (r != 0)
		goto exit;

	/* count parts and bind sections required by filters */
	struct filters_data fdata;
	memset(&fdata, 0, sizeof(fdata));
	r = filters_to_statement_and_bind(&fdata, filters, filter_size, 0, NULL, NULL);
	if (r != 0)
		goto exit;

	/* Allocate space for all required statement sections which will later be joined.
	 * base1 + interval1 + base2 + filters + aggregate + order + limit */
	const size_t aggregate_size = (ropts.flags & READ_OPTION_AGGREGATE_MASK) != 0
									? 1 : 0;
	const size_t interval_size = (ropts.flags & READ_OPTION_INTERVAL) == READ_OPTION_INTERVAL
									? 1 : 0;
	const size_t part_size = 1 + interval_size + 1 + fdata.part_count + aggregate_size + 1 + 1;
	parts = calloc(part_size ,sizeof(struct libowl_statement_part));
	if (parts == NULL) {
		r = -ENOMEM;
		goto exit;
	}

	/* Allocate space for all binding instructions to statement
	 * filters + interval + limit */
	const size_t bind_size = fdata.bind_count + interval_size + 1;
	bind = malloc(sizeof(struct libowl_bind) * bind_size);
	if (bind == NULL) {
		r = -ENOMEM;
		goto exit;
	}

	size_t parts_pos = 0;
	int bind_column = 1;
	size_t bind_pos = 0;

	/* base */
	switch (ropts.flags & READ_OPTION_AGGREGATE_MASK) {
	case READ_OPTION_AVG:
		parts[parts_pos++].str =
			"SELECT "
				"MAX(A.id),"
				"S.type_id,"
				"S.name,"
				"AVG(A.value),"
				"MAX(A.epoch)";
		break;
	case READ_OPTION_MIN:
		parts[parts_pos++].str =
			"SELECT "
				"MAX(A.id),"
				"S.type_id,"
				"S.name,"
				"MIN(A.value),"
				"MAX(A.epoch)";
		break;
	case READ_OPTION_MAX:
		parts[parts_pos++].str =
			"SELECT "
				"MAX(A.id),"
				"S.type_id,"
				"S.name,"
				"MAX(A.value), "
				"MAX(A.epoch)";
		break;
	default:
		parts[parts_pos++].str =
			"SELECT "
				"A.id,"
				"S.type_id,"
				"S.name,"
				"A.value,"
				"A.epoch";
		break;
	}

	if ((ropts.flags & READ_OPTION_INTERVAL) == READ_OPTION_INTERVAL) {
		parts[parts_pos++].str = ", CAST((A.epoch  * 1000 / (?)) AS INTEGER) AS interval";
		bind_double(&bind[bind_pos++], bind_column++, ropts.interval);
	}

	parts[parts_pos++].str =
		" FROM data as A"
		" INNER JOIN sensors AS S on S.id = A.sensor_id"
		" WHERE";

	/* filters  */
	r = filters_to_statement_and_bind(&fdata, filters, filter_size, bind_column, &parts[parts_pos], &bind[bind_pos]);
	if (r != 0)
		goto exit;
	bind_column += fdata.bind_count;
	bind_pos += fdata.bind_count;
	parts_pos += fdata.part_count;

	/* Aggegate grouping */
	if ((ropts.flags & READ_OPTION_AGGREGATE_MASK) != 0) {
		if ((ropts.flags & READ_OPTION_INTERVAL) == READ_OPTION_INTERVAL)
			parts[parts_pos++].str =" GROUP BY S.id, interval";
		else
			parts[parts_pos++].str =" GROUP BY S.id";
	}

	/* Add order */
	const int is_descending = (ropts.flags & READ_OPTION_DESCENDING) == READ_OPTION_DESCENDING;
	parts[parts_pos++].str = is_descending ? " ORDER BY A.id DESC" : " ORDER BY A.id ASC";

	/* Add limit */
	parts[parts_pos++].str = " LIMIT (?)";
	bind_int(&bind[bind_pos++], bind_column++, (int) size);

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
			data[pos].type = sqlite3_column_int(stmt, 1);
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
