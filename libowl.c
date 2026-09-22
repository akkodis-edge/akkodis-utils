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
	uint32_t interval_ms;
	struct timespec last_poll;
	struct timespec interval;
	struct libowl_sensor_ops ops;
	void *priv;
};

struct libowl {
	struct sqlite3 *db;
	int flags;
	struct libowl_sensor *sensors;
	size_t sensors_size;
	int (*monotonic)(struct timespec*, void*);
	void *monotonic_priv;
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

static int libowl_monotonic(struct timespec* time, void* priv)
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
	if ((owl->flags & LIBOWL_LOGLEVEL_DEBUG) == LIBOWL_LOGLEVEL_DEBUG) \
		{mprint(stderr, "libowl: error: " fmt, ##__VA_ARGS__);}

static int libowl_create_table(struct libowl* owl, const char* statement)
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
		pr_err(owl, "sqlite3_prepare_v2(insert_category) [%d]: %s\n", r, sqlite3_errstr(r));
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
		" PRAGMA journal_mode = WAL;"
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

	r = libowl_create_table(owl,
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

	r = libowl_create_table(owl,
		"CREATE TABLE IF NOT EXISTS sensors("
				"id INTEGER PRIMARY KEY,"
				"type_id INTEGER NOT NULL,"
				"name TEXT NOT NULL,"
				"FOREIGN KEY(type_id) REFERENCES category_type(id),"
				"UNIQUE(type_id, name)"
			") STRICT");
	if (r != 0)
		return r;

	/* Note: data(id) can be used to determine if time in data(epoch) has run backwards. */
	r = libowl_create_table(owl,
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

	newowl->monotonic = libowl_monotonic;
	newowl->flags = flags;

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

	*owl = newowl;
	newowl = NULL;
	r = 0;
exit:
	if (newowl != NULL)
		free(newowl);
	return r;
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
	free(owl);
	return 0;
}

static void timespec_from_ms(struct timespec* ts, int ms)
{
	ts->tv_sec = ms / 1000;
	ts->tv_nsec = (ms % 1000) * 1000000;
}

int libowl_add_sensor(struct libowl* owl, int type, const char* name, int flags, const struct libowl_sensor_ops* ops, int interval_ms, void* priv)
{
	if (owl == NULL || !is_write(owl) || libowl_sensor_type_str(type) == NULL || name == NULL || ops == NULL || interval_ms < 0)
		return -EINVAL;

	void *ptr = realloc(owl->sensors, sizeof(*owl->sensors) * (owl->sensors_size + 1));
	if (ptr == NULL)
		return -ENOMEM;
	owl->sensors = ptr;
	owl->sensors_size++;
	struct libowl_sensor *sensor = &owl->sensors[owl->sensors_size - 1];
	sensor->name = strdup(name);
	if (sensor->name == NULL) {
		owl->sensors_size--;
		return -ENOMEM;
	}
	sensor->type = type;
	sensor->flags = flags;
	timespec_from_ms(&sensor->interval, interval_ms);
	int r = owl->monotonic(&sensor->last_poll, owl->monotonic_priv);
	if (r != 0) {
		pr_err(owl, "owl->monotonic() [%d]: %s\n", r, strerror(r));
		goto exit;
	}
	memcpy(&sensor->ops, ops, sizeof(sensor->ops));
	sensor->priv = priv;

	sqlite3_stmt *stmt = NULL;
	r = sqlite3_prepare_v2(owl->db,
		"INSERT OR IGNORE INTO sensors(name, type_id) VALUES "
			"(?, (SELECT id from category_type WHERE name=(?)))",
		-1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = sqlite3_bind_text(stmt, 1, sensor->name, -1, SQLITE_STATIC);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_bind_text(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
	r = sqlite3_bind_text(stmt, 2, libowl_sensor_type_str(sensor->type), -1, SQLITE_STATIC);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_bind_text(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
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

/* Free string on failure and return NULL */
char* append_str(char** base, const char* append)
{
	const size_t base_len = *base == NULL ? 0 : strlen(*base);
	const size_t append_len = strlen(append);
	char *str = realloc(*base, base_len + append_len + 1);
	if (str == NULL) {
		free(*base);
		*base = NULL;
		return NULL;
	}
	*base = str;
	memcpy(*base + base_len, append, append_len + 1);
	return *base;
}

static int libowl_sensor_push(struct libowl* owl, struct libowl_sensor* sensor, int value)
{
	const int use_monotonic = (owl->flags & LIBOWL_TIMESTAMP_MONOTONIC) == LIBOWL_TIMESTAMP_MONOTONIC;
	double time = 0;

	if (use_monotonic) {
		struct timespec time_now;
		int r = owl->monotonic(&time_now, owl->monotonic_priv);
		if (r != 0)
			return r;
		time = (double) time_now.tv_sec + ((double) time_now.tv_nsec / 1.0e9);
	}

	char* sql = NULL;
	sql = append_str(&sql,
		"INSERT INTO data(sensor_id, value, epoch) VALUES "
			"((SELECT id from sensors WHERE type_id=(SELECT id from category_type WHERE name=(?)) AND name=(?)),"
			"?, ");
	if (sql == NULL)
		return -ENOMEM;
	sql = append_str(&sql,
		use_monotonic ? "(?))" : "unixepoch('now', 'subsec'))");
	if (sql == NULL)
		return -ENOMEM;

	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db, sql, -1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2(sensor_push) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	if (r == SQLITE_OK)
		r = sqlite3_bind_text(stmt, 1, libowl_sensor_type_str(sensor->type), -1, SQLITE_STATIC);
	if (r == SQLITE_OK)
		r = sqlite3_bind_text(stmt, 2, sensor->name, -1, SQLITE_STATIC);
	if (r == SQLITE_OK)
		r = sqlite3_bind_int(stmt, 3, value);
	if (r == SQLITE_OK && use_monotonic)
		r = sqlite3_bind_double(stmt, 4, time);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_bind(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = sqlite3_step(stmt);
	if (r != SQLITE_DONE) {
		pr_err(owl, "sqlite3_step(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = 0;
exit:
	free(sql);
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

int libowl_update(struct libowl* owl)
{
	if (owl == NULL || !is_write(owl))
		return -EINVAL;

	int sensors_updated = 0;
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
		if (r != 0) {
			pr_err(owl, "sensor->read [%d]: %s\n", r, strerror(r));
			return r;
		}
		memcpy(&owl->sensors[i].last_poll, &time_now, sizeof(owl->sensors[i].last_poll));
		r = libowl_sensor_push(owl, &owl->sensors[i], value);
		if (r != 0)
			return r;
		sensors_updated++;
	}
	return sensors_updated;
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

int libowl_read(struct libowl* owl, int flags, const struct libowl_filter* filters, size_t filter_size, struct libowl_sensor_data* data, size_t size)
{
	(void) flags;
	if (owl == NULL || filters == NULL || filter_size == 0 || data == NULL || size == 0 || size > INT_MAX)
		return -EINVAL;
	char *sql = NULL;
	sql = append_str(&sql,
			"SELECT "
				"A.id,"
				"(SELECT name from category_type WHERE id = S.type_id),"
				"S.name,"
				"A.value,"
				"A.epoch"
			" FROM data as A"
			" INNER JOIN sensors AS S on S.id = A.sensor_id"
			" WHERE");

	if (sql == NULL)
		return -ENOMEM;

	int pos = 0;

	for (size_t i = 0; i < filter_size; ++i) {
		const int buf_size = 64;
		char buf[buf_size];
		char *field = NULL;
		switch (filters[i].type) {
		case LIBOWL_FILTER_INDEX:
			field = "A.id";
			break;
		case LIBOWL_FILTER_EPOCH:
			field = "A.epoch";
			break;
		case LIBOWL_FILTER_NAME:
			field = "S.name";
			break;
		}
		if (field == NULL) {
			free(sql);
			return -EINVAL;
		}

		const int bytes = snprintf(buf, buf_size, " %s%s %s (?)",
				i > 0 ? "AND " : "", field, op_to_str(filters[i].op));
		if (bytes < 0) {
			free(sql);
			return -errno;
		}
		if (bytes >= buf_size) {
			free(sql);
			return -ENOMEM;
		}
		sql = append_str(&sql, buf);
		if (sql == NULL)
			return -ENOMEM;
	}

	const int is_descending = (flags & LIBOWL_READ_DESCENDING) == LIBOWL_READ_DESCENDING;
	sql = append_str(&sql, is_descending ? " ORDER BY A.id DESC" : " ORDER BY A.id ASC");
	if (sql == NULL)
		return -ENOMEM;
	sql = append_str(&sql, " LIMIT (?)");
	if (sql == NULL)
		return -ENOMEM;


	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db, sql, -1, &stmt, NULL);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_prepare_v2(read) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	int column = 1;

	for (size_t i = 0; i < filter_size; ++i) {
		r = SQLITE_NOTFOUND;
		switch (filters[i].type) {
		case LIBOWL_FILTER_INDEX:
			r = sqlite3_bind_int64(stmt, column, filters[i].data.mi64);
			break;
		case LIBOWL_FILTER_EPOCH:
			r = sqlite3_bind_double(stmt, column, filters[i].data.mdouble);
			break;
		case LIBOWL_FILTER_NAME:
			r = sqlite3_bind_text(stmt, 1, filters[i].data.str, -1, SQLITE_STATIC);
			break;
		}
		if (r == SQLITE_NOTFOUND) {
			r = -EINVAL;
			goto exit;
		}
		if (r != SQLITE_OK) {
			pr_err(owl, "sqlite3_bind(read) [%d]: %s\n", r, sqlite3_errstr(r));
			r = -EBADF;
			goto exit;
		}
		column++;
	}

	r = sqlite3_bind_int(stmt, column, (int) size);
	if (r != SQLITE_OK) {
		pr_err(owl, "sqlite3_bind_int(read) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}


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
	if (sql != NULL)
		free(sql);
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	if (r < 0) {
		for (int i = 0; i < pos; ++i)
			libowl_sensor_data_free(&data[i]);
	}
	return r;
}
