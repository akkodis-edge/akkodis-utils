#include <memory>
#include <cstring>
#include "libowl.h"
#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

struct test_data {
	struct libowl_sensor_data *data;
	size_t size;
};

struct Deleter {
	void operator()(struct libowl* owl)
	{
		libowl_close(owl);
	}
	void operator()(struct test_data* test)
	{
		for (size_t i = 0; i < test->size; ++i)
			libowl_sensor_data_free(&test->data[i]);
		free(test->data);
	}
};

static int dummy_sensor_read(int* value, void* priv)
{
	int *data = reinterpret_cast<int*>(priv);
	*value = *data;
	return 0;
}

static const struct libowl_sensor_ops dummy_ops {
	dummy_sensor_read,
};

static void timespec_from_ms(struct timespec* ts, int ms)
{
	ts->tv_sec = ms / 1000;
	ts->tv_nsec = (ms % 1000) * 1000000;
}

static int monotonic(struct timespec* ts, void* priv)
{
	int *time_ms = reinterpret_cast<int*>(priv);
	timespec_from_ms(ts, *time_ms);
	return 0;
}

TEST_CASE("single sensor") {
	struct libowl *owl = nullptr;
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE) == 0);
	auto at_exit = std::unique_ptr<struct libowl, Deleter>(owl);

	int time_ms = 0;
	REQUIRE(libowl_set_monotonic(owl, monotonic, &time_ms) == 0);

	int dummy_sensor_data = 99;
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "test", 0, &dummy_ops, 1000, &dummy_sensor_data) == 0);

	time_ms = 1000;
	REQUIRE(libowl_update(owl) == 1);

	struct libowl_sensor_data sdat {};
	struct libowl_filter filter;
	REQUIRE(libowl_filter_index(&filter, LIBOWL_OP_GREATER_EQUAL, 0) == 0);
	REQUIRE(libowl_read(owl, 0, &filter, 1, &sdat, 1) == 1);
	REQUIRE(strcmp(sdat.name, "test") == 0);
	REQUIRE(sdat.value == 99);
	REQUIRE(libowl_sensor_data_free(&sdat) == 0);
}

TEST_CASE("libowl_update_delay") {
	struct libowl *owl = nullptr;
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE) == 0);
	auto at_exit = std::unique_ptr<struct libowl, Deleter>(owl);

	int time_ms = 0;
	REQUIRE(libowl_set_monotonic(owl, monotonic, &time_ms) == 0);

	int dummy_sensor_data = 99;
	/* First sensor with interval 100ms */
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "test100", 0, &dummy_ops, 100, &dummy_sensor_data) == 0);
	/* Second sensor with interval 130ms */
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "test130", 0, &dummy_ops, 130, &dummy_sensor_data) == 0);

	/* No sensor has been polled and start time is 0,  should return 100 for test100 */
	REQUIRE(libowl_update_delay(owl) == 100);
	/* Move forward 100ms and poll test100 */
	time_ms += 100;
	REQUIRE(libowl_update(owl) == 1);
	/* Time until test130 is 30ms */
	REQUIRE(libowl_update_delay(owl) == 30);
	/* Moving forward 100ms should update both */
	time_ms += 100;
	REQUIRE(libowl_update(owl) == 2);
	/* Next is test100 */
	REQUIRE(libowl_update_delay(owl) == 100);
}

static int monotonic_ns(struct timespec* ts, void* priv)
{
	memcpy(ts, priv, sizeof(*ts));
	return 0;
}

TEST_CASE("libowl_update_delay round up nano to milli") {
	struct libowl *owl = nullptr;
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE) == 0);
	auto at_exit = std::unique_ptr<struct libowl, Deleter>(owl);

	struct timespec time_now;
	memset(&time_now, 0, sizeof(time_now));
	REQUIRE(libowl_set_monotonic(owl, monotonic_ns, &time_now) == 0);

	int dummy_sensor_data = 99;
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "test1", 0, &dummy_ops, 1, &dummy_sensor_data) == 0);
	REQUIRE(libowl_update_delay(owl) == 1);

	time_now.tv_nsec = (1000000 - 1);
	REQUIRE(libowl_update_delay(owl) == 1);
	time_now.tv_nsec++;
	REQUIRE(libowl_update_delay(owl) == 0);
}

static int monotonic_s(struct timespec* ts, void* priv)
{
	int *time_sec = reinterpret_cast<int*>(priv);
	ts->tv_sec = *time_sec;
	ts->tv_nsec = 0;
	return 0;
}

static std::unique_ptr<struct test_data, Deleter> prepare_data(struct test_data* test, size_t entries)
{
	test->size = entries;
	test->data = (struct libowl_sensor_data*) calloc(test->size, sizeof(*(test->data)));
	REQUIRE(test->data != nullptr);
	std::unique_ptr<struct test_data, Deleter> cleanup(test);
	return cleanup;
}

static void sensor_data_equal(const struct libowl_sensor_data* lhs, const struct libowl_sensor_data* rhs)
{
	REQUIRE(strcmp(lhs->name, rhs->name) == 0);
	REQUIRE(lhs->index == rhs->index);
	/* compare only seconds, drop fractions */
	REQUIRE((int64_t) lhs->epoch == (int64_t) rhs->epoch);
	REQUIRE(lhs->type == rhs->type);
	REQUIRE(lhs->value == rhs->value);
}

TEST_CASE("libowl_read") {
	/* Prepare database with separate entries, use our monotonic with second resolution to avoid issues with epoch double precision */
	struct libowl *owl = nullptr;
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE | LIBOWL_TIMESTAMP_MONOTONIC) == 0);
	auto at_exit = std::unique_ptr<struct libowl, Deleter>(owl);
	int seconds = 0;
	REQUIRE(libowl_set_monotonic(owl, monotonic_s, &seconds) == 0);

	int sensor1_value = 10;
	int sensor2_value = 20;
	int sensor3_value = 30;
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "sensor1", 0, &dummy_ops, 0, &sensor1_value) == 0);
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "sensor2", 0, &dummy_ops, 0, &sensor2_value) == 0);
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "sensor3", 0, &dummy_ops, 0, &sensor3_value) == 0);

	/* Add one reading for each sensor at three separate points in time */
	REQUIRE(libowl_update(owl) == 3);
	/* A second reading after 10 seconds */
	sensor1_value++;
	sensor2_value++;
	sensor3_value++;
	seconds = 10;
	REQUIRE(libowl_update(owl) == 3);
	/* A third reading after 10 more seconds */
	sensor1_value++;
	sensor2_value++;
	sensor3_value++;
	seconds = 20;
	REQUIRE(libowl_update(owl) == 3);
	/* 9 readings available in database */
	const size_t database_size = 9;
	/* expected data in database */
	const struct libowl_sensor_data expected[database_size] = {
		{"sensor1", 1, 0.0, LIBOWL_SENSOR_TEMP, 10},
		{"sensor2", 2, 0.0, LIBOWL_SENSOR_TEMP, 20},
		{"sensor3", 3, 0.0, LIBOWL_SENSOR_TEMP, 30},
		{"sensor1", 4, 10.0, LIBOWL_SENSOR_TEMP, 11},
		{"sensor2", 5, 10.0, LIBOWL_SENSOR_TEMP, 21},
		{"sensor3", 6, 10.0, LIBOWL_SENSOR_TEMP, 31},
		{"sensor1", 7, 20.0, LIBOWL_SENSOR_TEMP, 12},
		{"sensor2", 8, 20.0, LIBOWL_SENSOR_TEMP, 22},
		{"sensor3", 9, 20.0, LIBOWL_SENSOR_TEMP, 32},
	};

	SECTION("Filter by index -- all") {
		struct test_data test;
		auto cleanup = prepare_data(&test, database_size + 1);
		struct libowl_filter filter;
		REQUIRE(libowl_filter_index(&filter, LIBOWL_OP_GREATER_EQUAL, 1) == 0);
		REQUIRE(libowl_read(owl, 0, &filter, 1, test.data, test.size) == 9);
		for (size_t i = 0; i < database_size; ++i)
			sensor_data_equal(&test.data[i], &expected[i]);
	}

	SECTION("Filter by index -- all, descending") {
		struct test_data test;
		auto cleanup = prepare_data(&test, database_size + 1);
		struct libowl_filter filter;
		REQUIRE(libowl_filter_index(&filter, LIBOWL_OP_GREATER_EQUAL, 1) == 0);
		REQUIRE(libowl_read(owl, LIBOWL_READ_DESCENDING, &filter, 1, test.data, test.size) == 9);
		for (size_t i = 0; i < database_size; ++i)
			sensor_data_equal(&test.data[i], &expected[database_size - 1 - i]);
	}

	SECTION("Filter by index -- middle three") {
		struct test_data test;
		auto cleanup = prepare_data(&test, 4);
		struct libowl_filter filters[2];
		REQUIRE(libowl_filter_index(&filters[0], LIBOWL_OP_GREATER_THAN, 3) == 0);
		REQUIRE(libowl_filter_index(&filters[1], LIBOWL_OP_LESS_THAN, 7) == 0);
		REQUIRE(libowl_read(owl, 0, filters, 2, test.data, test.size) == 3);
		sensor_data_equal(&test.data[0], &expected[3]);
		sensor_data_equal(&test.data[1], &expected[4]);
		sensor_data_equal(&test.data[2], &expected[5]);
	}

	SECTION("Filter by epoch -- all") {
		struct test_data test;
		auto cleanup = prepare_data(&test, database_size + 1);
		struct libowl_filter filter;
		REQUIRE(libowl_filter_epoch(&filter, LIBOWL_OP_GREATER_EQUAL, 0.0) == 0);
		REQUIRE(libowl_read(owl, 0, &filter, 1, test.data, test.size) == 9);
		for (size_t i = 0; i < database_size; ++i)
			sensor_data_equal(&test.data[i], &expected[i]);
	}

	SECTION("Filter by epoch -- middle three") {
		struct test_data test;
		auto cleanup = prepare_data(&test, 4);
		struct libowl_filter filters[2];
		REQUIRE(libowl_filter_epoch(&filters[0], LIBOWL_OP_GREATER_THAN, 0.0) == 0);
		REQUIRE(libowl_filter_epoch(&filters[1], LIBOWL_OP_LESS_THAN, 20.0) == 0);
		REQUIRE(libowl_read(owl, 0, filters, 2, test.data, test.size) == 3);
		sensor_data_equal(&test.data[0], &expected[3]);
		sensor_data_equal(&test.data[1], &expected[4]);
		sensor_data_equal(&test.data[2], &expected[5]);
	}

	SECTION("Filter by name -- sensor1") {
		struct test_data test;
		auto cleanup = prepare_data(&test, database_size + 1);
		struct libowl_filter filter;
		REQUIRE(libowl_filter_name(&filter, LIBOWL_OP_EQUAL, "sensor1") == 0);
		REQUIRE(libowl_read(owl, 0, &filter, 1, test.data, test.size) == 3);
		sensor_data_equal(&test.data[0], &expected[0]);
		sensor_data_equal(&test.data[1], &expected[3]);
		sensor_data_equal(&test.data[2], &expected[6]);
	}

	SECTION("Filter by name -- find all sensors") {
		struct test_data test;
		auto cleanup = prepare_data(&test, database_size + 1);
		struct libowl_filter filter;
		REQUIRE(libowl_filter_name(&filter, LIBOWL_OP_GREATER_THAN, "") == 0);
		REQUIRE(libowl_read(owl, 0, &filter, 1, &test.data[0], 1) == 1);
		sensor_data_equal(&test.data[0], &expected[0]);
		REQUIRE(libowl_filter_name(&filter, LIBOWL_OP_GREATER_THAN, test.data[0].name) == 0);
		REQUIRE(libowl_read(owl, 0, &filter, 1, &test.data[1], 1) == 1);
		sensor_data_equal(&test.data[1], &expected[1]);
		REQUIRE(libowl_filter_name(&filter, LIBOWL_OP_GREATER_THAN, test.data[1].name) == 0);
		REQUIRE(libowl_read(owl, 0, &filter, 1, &test.data[2], 1) == 1);
		sensor_data_equal(&test.data[2], &expected[2]);
		REQUIRE(libowl_filter_name(&filter, LIBOWL_OP_GREATER_THAN, test.data[2].name) == 0);
		REQUIRE(libowl_read(owl, 0, &filter, 1, &test.data[3], 1) == 0);
	}
}

TEST_CASE("buffer_duration") {
	/* Prepare database */
	struct libowl *owl = nullptr;
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE) == 0);
	auto at_exit = std::unique_ptr<struct libowl, Deleter>(owl);
	int time_ms = 0;
	REQUIRE(libowl_set_monotonic(owl, monotonic, &time_ms) == 0);

	/* Add one sensor */
	int sensor1_value = 10;
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "sensor1", 0, &dummy_ops, 10, &sensor1_value) == 0);

	/* enable buffering, 1000ms */
	libowl_set_buffer_duration(owl, 1000);

	/* Add three readings to buffer */
	const int count = 3;
	for (int i = 0; i < count; ++i) {
		time_ms += 10;
		REQUIRE(libowl_update(owl) == 0);
	}

	SECTION("buffer reset") {
		libowl_set_buffer_duration(owl, 0);
		REQUIRE(libowl_update(owl) == count);
	}

	SECTION("normal operation") {
		/* write buffer and one more entry */
		time_ms += 1000;
		REQUIRE(libowl_update(owl) == count + 1);

		/* buffer timer should not be started as sensor is not ready for reading */
		time_ms += 5;
		REQUIRE(libowl_update(owl) == 0);
		/* start timer and read to buffer */
		time_ms += 5;
		REQUIRE(libowl_update(owl) == 0);
		/* still buffering */
		time_ms += 990;
		REQUIRE(libowl_update(owl) == 0);
		/* new reading and write buffer */
		time_ms += 10;
		REQUIRE(libowl_update(owl) == 3);

		/* buffer 5s */
		libowl_set_buffer_duration(owl, 5);
		/* do not start buffer counter due to no reading */
		REQUIRE(libowl_update(owl) == 0);
		/* time to read 1 sensor to buffer */
		time_ms += 10;
		REQUIRE(libowl_update(owl) == 0);
		/* no update */
		time_ms += 4;
		REQUIRE(libowl_update(owl) == 0);
		/* write buffer */
		time_ms += 1;
		REQUIRE(libowl_update(owl) == 1);
	}
}
