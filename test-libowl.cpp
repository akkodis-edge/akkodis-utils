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

static int read_inc(int* value, void* priv)
{
	int *data = reinterpret_cast<int*>(priv);
	*value = *data;
	(*data)++;
	return 0;
}


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


TEST_CASE("Add sensor") {
	struct libowl *owl = nullptr;
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE | LIBOWL_LOGLEVEL_DEBUG) == 0);
	auto at_exit = std::unique_ptr<struct libowl, Deleter>(owl);

	int time_ms = 0;
	REQUIRE(libowl_set_monotonic(owl, monotonic, &time_ms) == 0);

	struct libowl_sensor_ops ops;
	ops.read = read_inc;
	int data = 99;
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "test", 0, &ops, 1000, &data) == 0);

	time_ms = 1001;
	REQUIRE(libowl_next(owl, 0) == 1);

	struct libowl_sensor_data sdat {};
	size_t sdat_size = 1;
	REQUIRE(libowl_read(owl, 0, &sdat, &sdat_size) == 0);
	REQUIRE(strcmp(sdat.name, "test") == 0);
	REQUIRE(sdat.value == 99);
	REQUIRE(libowl_sensor_data_free(&sdat) == 0);
}

