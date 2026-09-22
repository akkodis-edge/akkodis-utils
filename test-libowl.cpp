#include <memory>
#include <cstring>
#include "libowl.h"
#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

struct Deleter {
	void operator()(struct libowl* owl)
	{
		libowl_close(owl);
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
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE | LIBOWL_LOGLEVEL_DEBUG) == 0);
	auto at_exit = std::unique_ptr<struct libowl, Deleter>(owl);

	int time_ms = 0;
	REQUIRE(libowl_set_monotonic(owl, monotonic, &time_ms) == 0);

	int dummy_sensor_data = 99;
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "test", 0, &dummy_ops, 1000, &dummy_sensor_data) == 0);

	time_ms = 1000;
	REQUIRE(libowl_update(owl) == 1);

	struct libowl_sensor_data sdat {};
	size_t sdat_size = 1;
	REQUIRE(libowl_read(owl, 0, nullptr, 0, &sdat, &sdat_size) == 0);
	REQUIRE(strcmp(sdat.name, "test") == 0);
	REQUIRE(sdat.value == 99);
	REQUIRE(libowl_sensor_data_free(&sdat) == 0);
}

TEST_CASE("libowl_update_delay") {
	struct libowl *owl = nullptr;
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE | LIBOWL_LOGLEVEL_DEBUG) == 0);
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
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE | LIBOWL_LOGLEVEL_DEBUG) == 0);
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

