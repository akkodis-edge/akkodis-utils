#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
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

/* Return 0 for duration not elapsed, 1 for duration elapsed or negative errno for error */
static int libowl_duration_elapsed(const struct timespec* time_now, const struct timespec* start, const struct timespec* duration)
{
	struct timespec elapsed = {
			.tv_sec = time_now->tv_sec - start->tv_sec,
			.tv_nsec = time_now->tv_nsec - start->tv_nsec};
	if (elapsed.tv_nsec < 0) {
		elapsed.tv_sec--;
		elapsed.tv_nsec += 1000000000L;
	}
	if (elapsed.tv_sec == duration->tv_sec)
		return elapsed.tv_nsec > duration->tv_nsec;
	else
		return elapsed.tv_sec > duration->tv_sec;
}

static int libowl_monotonic(struct timespec* ts, void* priv)
{
	(void) priv;
	const int r = clock_gettime(CLOCK_MONOTONIC, ts);
	if (r != 0)
		return -errno;
	return 0;
}

static int is_debug(const struct libowl* owl)
{
	return (owl->flags & LIBOWL_LOGLEVEL_DEBUG) == LIBOWL_LOGLEVEL_DEBUG;
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
	}
	return NULL;
}

static int libowl_create_table(struct libowl* owl, const char* statement)
{
	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db, statement, -1, &stmt, NULL);
	if (r != SQLITE_OK) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_prepare_v2(create_table) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = sqlite3_step(stmt);
	if (r != SQLITE_DONE) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_step(create_table) [%d]: %s\n", r, sqlite3_errstr(r));
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
		if (is_debug(owl))
			printf("libowl: error: sqlite3_prepare_v2(insert_category) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	for (int i = 0; i <= LIBOWL_SENSOR_TEMP; ++i) {
		r = sqlite3_bind_text(stmt, 1, libowl_sensor_type_str(LIBOWL_SENSOR_TEMP), -1, SQLITE_STATIC);
		if (r != SQLITE_OK) {
			if (is_debug(owl))
				printf("libowl: error: sqlite3_bind_text(category) [%d]: %s\n", r, sqlite3_errstr(r));
			r = -EBADF;
			goto exit;
		}
		r = sqlite3_step(stmt);
		if (r != SQLITE_DONE) {
			if (is_debug(owl))
				printf("libowl: error: sqlite3_step(category) [%d]: %s\n", r, sqlite3_errstr(r));
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

static int libowl_init_database(struct libowl* owl)
{
	int r = libowl_create_table(owl,
		"CREATE TABLE IF NOT EXISTS category_type("
			"id INTEGER PRIMARY KEY,"
			"name TEXT NOT NULL,"
		   "UNIQUE(name)"
		")");
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
			")");
	if (r != 0)
		return r;

	/* Note: data(id) can be used to determine if time in data(epoch) has run backwards. */
	r = libowl_create_table(owl,
		"CREATE TABLE IF NOT EXISTS data("
				"id INTEGER PRIMARY KEY,"
				"sensor_id INTEGER NOT NULL,"
				"value INTEGER NOT NULL,"
				"epoch INTEGER NOT NULL,"
				"FOREIGN KEY(sensor_id) REFERENCES sensors(id)"
			")");
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
		if (is_debug(newowl))
			printf("libowl: error: sqlite3_open_v2() [%d]: %s\n", r, sqlite3_errstr(r));
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
		if (is_debug(owl))
			printf("libowl: error: owl->monotonic() [%d]: %s\n", r, strerror(r));
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
		if (is_debug(owl))
			printf("libowl: error: sqlite3_prepare_v2(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = sqlite3_bind_text(stmt, 1, sensor->name, -1, SQLITE_STATIC);
	if (r != SQLITE_OK) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_bind_text(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
	r = sqlite3_bind_text(stmt, 2, libowl_sensor_type_str(sensor->type), -1, SQLITE_STATIC);
	if (r != SQLITE_OK) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_bind_text(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
	r = sqlite3_step(stmt);
	if (r != SQLITE_DONE) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_step(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
	r = 0;
exit:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	return r;
}

static int libowl_sensor_push(struct libowl* owl, struct libowl_sensor* sensor, int value)
{
	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db,
		"INSERT INTO data(sensor_id, value, epoch) VALUES "
			"((SELECT id from sensors WHERE type_id=(SELECT id from category_type WHERE name=(?)) AND name=(?)),"
			"?, unixepoch('now'))",
		-1, &stmt, NULL);
	if (r != SQLITE_OK) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_prepare_v2(sensor_push) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = sqlite3_bind_text(stmt, 1, libowl_sensor_type_str(sensor->type), -1, SQLITE_STATIC);
	if (r != SQLITE_OK) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_bind_text(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
	r = sqlite3_bind_text(stmt, 2, sensor->name, -1, SQLITE_STATIC);
	if (r != SQLITE_OK) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_bind_text(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
	r = sqlite3_bind_int(stmt, 3, value);
	if (r != SQLITE_OK) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_bind_int(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
	r = sqlite3_step(stmt);
	if (r != SQLITE_DONE) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_step(insert_sensor) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = 0;
exit:
	if (stmt != NULL)
		sqlite3_finalize(stmt);
	return r;
}

int libowl_next(struct libowl* owl, int timeout_ms)
{
	if (owl == NULL || !is_write(owl))
		return -EINVAL;

	struct timespec timeout;
	memset(&timeout, 0, sizeof(timeout));
	if (timeout_ms > 0)
		timespec_from_ms(&timeout, timeout_ms);
	struct timespec start;
	int r = owl->monotonic(&start, owl->monotonic_priv);
	if (r != 0) {
		if (is_debug(owl))
			printf("libowl: error: owl->monotonic() [%d]: %s\n", r, strerror(r));
		return r;
	}

	while (1) {
		int read_sensors = 0;
		struct timespec time_now;
		r = owl->monotonic(&time_now, owl->monotonic_priv);
		if (r != 0)
			return r;

		for (size_t i = 0; i < owl->sensors_size; ++i) {
			if (libowl_duration_elapsed(&time_now, &owl->sensors[i].last_poll, &owl->sensors[i].interval) > 0) {
				int value = 0;
				r = owl->sensors[i].ops.read(&value, owl->sensors[i].priv);
				if (r != 0) {
					if (is_debug(owl))
						printf("libowl: error: sensor->read [%d]: %s\n", r, strerror(r));
					return r;
				}
				memcpy(&owl->sensors[i].last_poll, &time_now, sizeof(owl->sensors[i].last_poll));
				r = libowl_sensor_push(owl, &owl->sensors[i], value);
				if (r != 0)
					return r;
				read_sensors++;
			}
		}

		if (timeout_ms == 0) /* non-blocking */
			return read_sensors;
		if (timeout_ms < 0 && read_sensors > 0) /* blocking */
			return read_sensors;
		if (timeout_ms > 0 && libowl_duration_elapsed(&time_now, &start, &timeout) > 0)
			return read_sensors;
	}
	return 0;
}

int libowl_read(struct libowl* owl, int64_t index, struct libowl_sensor_data* data, size_t* size)
{
	if (owl == NULL || data == NULL || *size = 0 || *size > INT64_MAX)
		return -EINVAL;

	sqlite3_stmt *stmt = NULL;
	int r = sqlite3_prepare_v2(owl->db,
		"SELECT"
			"A.id,"
			"(SELECT name from category_type WHERE id = S.type_id),"
			"S.name,"
			"A.value,"
			"A.epoch"
		"FROM data as A"
		"INNER JOIN sensors AS S on S.id = A.sensor_id"
		"WHERE A.id >= (?)"
		"ORDER BY A.id ASC"
		"LIMIT (?)"
		-1, &stmt, NULL);
	if (r != SQLITE_OK) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_prepare_v2(read) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	r = sqlite3_bind_int64(stmt, 1, index);
	if (r != SQLITE_OK) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_bind_int64(read) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}
	r = sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(*size));
	if (r != SQLITE_OK) {
		if (is_debug(owl))
			printf("libowl: error: sqlite3_bind_int64(read) [%d]: %s\n", r, sqlite3_errstr(r));
		r = -EBADF;
		goto exit;
	}

	size_t pos = 0;
	r = SQLITE_ERROR;
	do {
		r = sqlite3_step(stmt);
		switch (r) {
		case SQLITE_DONE:
			break;
		case SQLITE_ROW:
			data[pos].stmt.column_int64()
			page.data.push_back(unit_from_statement(s, 0));
			break;
		default:
			if (is_debug(owl))
				printf("libowl: error: sqlite3_step(read) [%d]: %s\n", r, sqlite3_errstr(r));
		}
	} while (r != SQLITE_DONE);


# Read data since id
res = cursor.execute('''

''', (id,))
return res.fetchall()

}
