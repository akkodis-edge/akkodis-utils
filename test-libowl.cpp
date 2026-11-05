#include <memory>
#include <vector>
#include <array>
#include <cstring>
#include "libowl.h"
#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

struct SensorData {
public:
	SensorData() = default;
	SensorData(size_t size)
		: data (size)
	{}
	SensorData(const SensorData&) = delete;
	SensorData& operator=(const SensorData&) = delete;
	SensorData(SensorData&&) = default;
	SensorData& operator=(SensorData&&) = default;
	SensorData(SensorData&) = default;
	~SensorData()
	{
		for (auto& sensor : data)
			libowl_sensor_data_free(&sensor);
	}
	std::vector<struct libowl_sensor_data> data;
};

struct Deleter {
	void operator()(struct libowl* owl)
	{
		libowl_close(owl);
	}
	void operator()(struct libowl_sensor_data* data)
	{
		libowl_sensor_data_free(data);
	}
	void operator()(struct libowl_datapoints* dp)
	{
		libowl_datapoints_free(dp);
	}
	void operator()(struct libowl_impl_dp* idp)
	{
		libowl_impl_dp_free(idp);
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

static int monotonic(struct timespec* ts, void* priv)
{
	int *time_ms = reinterpret_cast<int*>(priv);
	timespec_from_ms(ts, *time_ms);
	return 0;
}

struct SensorType {
	const char* name;
	int type;
};

TEST_CASE("sensor types") {
	struct libowl *owl = nullptr;
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE) == 0);
	auto at_exit = std::unique_ptr<struct libowl, Deleter>(owl);

	const std::vector<SensorType> sensors = {
		{.name = "TEMP", .type = LIBOWL_SENSOR_TEMP},
		{.name = "VOLTAGE", .type = LIBOWL_SENSOR_VOLTAGE},
		{.name = "CURRENT", .type = LIBOWL_SENSOR_CURRENT},
		{.name = "RATIO", .type = LIBOWL_SENSOR_RATIO},
		{.name = "COUNTER", .type = LIBOWL_SENSOR_COUNTER},
	};
	for (const auto& sensor : sensors) {
		/* confirm name */
		REQUIRE(strcmp(libowl_sensor_type_str(sensor.type), sensor.name) == 0);
		/* Add sensor */
		int dummy_sensor_data = 0;
		REQUIRE(libowl_add_sensor(owl, sensor.type, libowl_sensor_type_str(sensor.type), 0, &dummy_ops, 0, &dummy_sensor_data) == 0);
		/* Add single reading */
		REQUIRE(libowl_update(owl) > 0);
		/* retrieve value */
		struct libowl_sensor_data sdat {};
		auto at_data_exit = std::unique_ptr<struct libowl_sensor_data, Deleter>(&sdat);
		struct libowl_filter filter {};
		REQUIRE(libowl_filter_name(&filter, LIBOWL_OP_EQUAL, libowl_sensor_type_str(sensor.type)) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, &filter, 1, &sdat, 1) == 1);
		REQUIRE(sdat.type == sensor.type);
	}
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
	struct libowl_filter filter {};
	REQUIRE(libowl_filter_index(&filter, LIBOWL_OP_GREATER_EQUAL, 0) == 0);
	REQUIRE(libowl_read(owl, nullptr, 0, &filter, 1, &sdat, 1) == 1);
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

	struct timespec time_now {};
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

inline bool operator==(const struct libowl_sensor_data& lhs, const struct libowl_sensor_data& rhs)
{
	return strcmp(lhs.name, rhs.name) == 0 && lhs.index == rhs.index
			&& lhs.type == rhs.type && lhs.value == rhs.value
			/* compare only seconds, drop fractions */
			&& static_cast<int64_t>(lhs.epoch) == static_cast<int64_t>(rhs.epoch);
}
inline bool operator!=(const struct libowl_sensor_data& lhs, const struct libowl_sensor_data& rhs) {return !(lhs == rhs);}

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
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_VOLTAGE, "sensor2", 0, &dummy_ops, 0, &sensor2_value) == 0);
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_CURRENT, "sensor3", 0, &dummy_ops, 0, &sensor3_value) == 0);

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
	/* expected data in database */
	const std::vector<struct libowl_sensor_data> expected = {
		{.name = "sensor1", .index = 1, .epoch = 0.0, .type = LIBOWL_SENSOR_TEMP, .value = 10},
		{.name = "sensor2", .index = 2, .epoch = 0.0, .type = LIBOWL_SENSOR_VOLTAGE, .value = 20},
		{.name = "sensor3", .index = 3, .epoch = 0.0, .type = LIBOWL_SENSOR_CURRENT, .value = 30},
		{.name = "sensor1", .index = 4, .epoch = 10.0, .type = LIBOWL_SENSOR_TEMP, .value = 11},
		{.name = "sensor2", .index = 5, .epoch = 10.0, .type = LIBOWL_SENSOR_VOLTAGE, .value = 21},
		{.name = "sensor3", .index = 6, .epoch = 10.0, .type = LIBOWL_SENSOR_CURRENT, .value = 31},
		{.name = "sensor1", .index = 7, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = 12},
		{.name = "sensor2", .index = 8, .epoch = 20.0, .type = LIBOWL_SENSOR_VOLTAGE, .value = 22},
		{.name = "sensor3", .index = 9, .epoch = 20.0, .type = LIBOWL_SENSOR_CURRENT, .value = 32},
	};

	SECTION("Filter by index -- all") {
		SensorData returned(expected.size() + 1);
		struct libowl_filter filter {};
		REQUIRE(libowl_filter_index(&filter, LIBOWL_OP_GREATER_EQUAL, 1) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, &filter, 1, returned.data.data(), returned.data.size()) == static_cast<int>(expected.size()));
		REQUIRE(std::equal(expected.begin(), expected.end(), returned.data.begin()));
	}

	SECTION("Filter by index -- all, descending") {
		SensorData returned(expected.size() + 1);
		struct libowl_filter filter {};
		REQUIRE(libowl_filter_index(&filter, LIBOWL_OP_GREATER_EQUAL, 1) == 0);
		struct libowl_option option {};
		REQUIRE(libowl_option_descending(&option) == 0);
		REQUIRE(libowl_read(owl, &option, 1, &filter, 1, returned.data.data(), returned.data.size()) == static_cast<int>(expected.size()));
		REQUIRE(std::equal(expected.rbegin(), expected.rend(), returned.data.begin()));
	}

	SECTION("Filter by index -- middle three") {
		SensorData returned(4);
		std::array<struct libowl_filter, 2> filters;
		REQUIRE(libowl_filter_index(&filters[0], LIBOWL_OP_GREATER_THAN, 3) == 0);
		REQUIRE(libowl_filter_index(&filters[1], LIBOWL_OP_LESS_THAN, 7) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, filters.data(), filters.size(), returned.data.data(), returned.data.size()) == 3);
		REQUIRE(returned.data.at(0) == expected.at(3));
		REQUIRE(returned.data.at(1) == expected.at(4));
		REQUIRE(returned.data.at(2) == expected.at(5));
	}

	SECTION("Filter by index -- in") {
		SensorData returned(3);
		std::array<struct libowl_filter, 2> filters;
		REQUIRE(libowl_filter_index(&filters[0], LIBOWL_OP_IN, 2) == 0);
		REQUIRE(libowl_filter_index(&filters[1], LIBOWL_OP_IN, 6) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, filters.data(), filters.size(), returned.data.data(), returned.data.size()) == 2);
		REQUIRE(returned.data.at(0) == expected.at(1));
		REQUIRE(returned.data.at(1) == expected.at(5));
	}

	SECTION("Filter by epoch -- all") {
		SensorData returned(expected.size() + 1);
		struct libowl_filter filter {};
		REQUIRE(libowl_filter_epoch(&filter, LIBOWL_OP_GREATER_EQUAL, 0.0) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, &filter, 1, returned.data.data(), returned.data.size()) == static_cast<int>(expected.size()));
		REQUIRE(std::equal(expected.begin(), expected.end(), returned.data.begin()));
	}

	SECTION("Filter by epoch -- middle three") {
		SensorData returned(4);
		std::array<struct libowl_filter, 2> filters;
		REQUIRE(libowl_filter_epoch(&filters[0], LIBOWL_OP_GREATER_THAN, 0.0) == 0);
		REQUIRE(libowl_filter_epoch(&filters[1], LIBOWL_OP_LESS_THAN, 20.0) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, filters.data(), filters.size(), returned.data.data(), returned.data.size()) == 3);
		REQUIRE(returned.data.at(0) == expected.at(3));
		REQUIRE(returned.data.at(1) == expected.at(4));
		REQUIRE(returned.data.at(2) == expected.at(5));
	}

	SECTION("Filter by epoch -- in") {
		SensorData returned(7);
		std::array<struct libowl_filter, 2> filters;
		REQUIRE(libowl_filter_epoch(&filters[0], LIBOWL_OP_IN, 0.0) == 0);
		REQUIRE(libowl_filter_epoch(&filters[1], LIBOWL_OP_IN, 20.0) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, filters.data(), filters.size(), returned.data.data(), returned.data.size()) == 6);
		REQUIRE(returned.data.at(0) == expected.at(0));
		REQUIRE(returned.data.at(1) == expected.at(1));
		REQUIRE(returned.data.at(2) == expected.at(2));
		REQUIRE(returned.data.at(3) == expected.at(6));
		REQUIRE(returned.data.at(4) == expected.at(7));
		REQUIRE(returned.data.at(5) == expected.at(8));
	}

	SECTION("Filter by name -- sensor1") {
		SensorData returned(4);
		struct libowl_filter filter {};
		REQUIRE(libowl_filter_name(&filter, LIBOWL_OP_EQUAL, "sensor1") == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, &filter, 1, returned.data.data(), returned.data.size()) == 3);
		REQUIRE(returned.data.at(0) == expected.at(0));
		REQUIRE(returned.data.at(1) == expected.at(3));
		REQUIRE(returned.data.at(2) == expected.at(6));
	}


	SECTION("Filter by name -- not sensor1") {
		SensorData returned(7);
		struct libowl_filter filter {};
		REQUIRE(libowl_filter_name(&filter, LIBOWL_OP_NOT_EQUAL, "sensor1") == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, &filter, 1, returned.data.data(), returned.data.size()) == 6);
		REQUIRE(returned.data.at(0) == expected.at(1));
		REQUIRE(returned.data.at(1) == expected.at(2));
		REQUIRE(returned.data.at(2) == expected.at(4));
		REQUIRE(returned.data.at(3) == expected.at(5));
		REQUIRE(returned.data.at(4) == expected.at(7));
		REQUIRE(returned.data.at(5) == expected.at(8));
	}

	SECTION("Filter by name -- in") {
		SensorData returned(7);
		std::array<struct libowl_filter, 2> filters;
		REQUIRE(libowl_filter_name(&filters[0], LIBOWL_OP_IN, "sensor1") == 0);
		REQUIRE(libowl_filter_name(&filters[1], LIBOWL_OP_IN, "sensor3") == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, filters.data(), filters.size(), returned.data.data(), returned.data.size()) == 6);
		REQUIRE(returned.data.at(0) == expected.at(0));
		REQUIRE(returned.data.at(1) == expected.at(2));
		REQUIRE(returned.data.at(2) == expected.at(3));
		REQUIRE(returned.data.at(3) == expected.at(5));
		REQUIRE(returned.data.at(4) == expected.at(6));
		REQUIRE(returned.data.at(5) == expected.at(8));
	}

	SECTION("Filter by type -- all") {
		SensorData returned(10);
		struct libowl_filter filter {};
		REQUIRE(libowl_filter_type(&filter, LIBOWL_OP_GREATER_EQUAL, LIBOWL_SENSOR_TEMP) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, &filter, 1, returned.data.data(), returned.data.size()) == 9);
		REQUIRE(std::equal(expected.begin(), expected.end(), returned.data.begin()));
	}

	SECTION("Filter by type -- voltage") {
		SensorData returned(4);
		struct libowl_filter filter {};
		REQUIRE(libowl_filter_type(&filter, LIBOWL_OP_EQUAL, LIBOWL_SENSOR_VOLTAGE) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, &filter, 1, returned.data.data(), returned.data.size()) == 3);
		REQUIRE(returned.data.at(0) == expected.at(1));
		REQUIRE(returned.data.at(1) == expected.at(4));
		REQUIRE(returned.data.at(2) == expected.at(7));
	}

	SECTION("Filter by type -- in") {
		SensorData returned(7);
		std::array<struct libowl_filter, 2> filters;
		REQUIRE(libowl_filter_type(&filters[0], LIBOWL_OP_IN, LIBOWL_SENSOR_VOLTAGE) == 0);
		REQUIRE(libowl_filter_type(&filters[1], LIBOWL_OP_IN, LIBOWL_SENSOR_CURRENT) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, filters.data(), filters.size(), returned.data.data(), returned.data.size()) == 6);
		REQUIRE(returned.data.at(0) == expected.at(1));
		REQUIRE(returned.data.at(1) == expected.at(2));
		REQUIRE(returned.data.at(2) == expected.at(4));
		REQUIRE(returned.data.at(3) == expected.at(5));
		REQUIRE(returned.data.at(4) == expected.at(7));
		REQUIRE(returned.data.at(5) == expected.at(8));
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

TEST_CASE("read aggregate") {
	/* Prepare database with separate entries, use our monotonic with second resolution to avoid issues with epoch double precision */
	struct libowl *owl = nullptr;
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE | LIBOWL_TIMESTAMP_MONOTONIC) == 0);
	auto at_exit = std::unique_ptr<struct libowl, Deleter>(owl);
	libowl_set_loglevel(owl, LIBOWL_LOGLEVEL_ERROR);
	int seconds = 0;
	REQUIRE(libowl_set_monotonic(owl, monotonic_s, &seconds) == 0);

	const int sensor_1_multiple = 10;
	const int sensor_2_multiple = 100;
	const int sensor_3_multiple = 1000;

	int sensor1_value = sensor_1_multiple;
	int sensor2_value = sensor_2_multiple;
	int sensor3_value = sensor_3_multiple;
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "sensor1", 0, &dummy_ops, 0, &sensor1_value) == 0);
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "sensor2", 0, &dummy_ops, 0, &sensor2_value) == 0);
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "sensor3", 0, &dummy_ops, 0, &sensor3_value) == 0);

	/* Add nine readings for each sensor at three separate timepoints */
	for (int i = 0; i < 9; ++i) {
		REQUIRE(libowl_update(owl) == 3);
		seconds += 10;
		sensor1_value += sensor_1_multiple;
		sensor2_value += sensor_2_multiple;
		sensor3_value += sensor_3_multiple;
	}

	/* expected data in database */
	const std::vector<struct libowl_sensor_data> expected = {
		{.name = "sensor1", .index = 1, .epoch = 0.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_1_multiple * 1},
		{.name = "sensor2", .index = 2, .epoch = 0.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_2_multiple * 1},
		{.name = "sensor3", .index = 3, .epoch = 0.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_3_multiple * 1},
		{.name = "sensor1", .index = 4, .epoch = 10.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_1_multiple * 2},
		{.name = "sensor2", .index = 5, .epoch = 10.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_2_multiple * 2},
		{.name = "sensor3", .index = 6, .epoch = 10.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_3_multiple * 2},
		{.name = "sensor1", .index = 7, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_1_multiple * 3},
		{.name = "sensor2", .index = 8, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_2_multiple * 3},
		{.name = "sensor3", .index = 9, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_3_multiple * 3},
		{.name = "sensor1", .index = 10, .epoch = 30.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_1_multiple * 4},
		{.name = "sensor2", .index = 11, .epoch = 30.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_2_multiple * 4},
		{.name = "sensor3", .index = 12, .epoch = 30.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_3_multiple * 4},
		{.name = "sensor1", .index = 13, .epoch = 40.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_1_multiple * 5},
		{.name = "sensor2", .index = 14, .epoch = 40.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_2_multiple * 5},
		{.name = "sensor3", .index = 15, .epoch = 40.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_3_multiple * 5},
		{.name = "sensor1", .index = 16, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_1_multiple * 6},
		{.name = "sensor2", .index = 17, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_2_multiple * 6},
		{.name = "sensor3", .index = 18, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_3_multiple * 6},
		{.name = "sensor1", .index = 19, .epoch = 60.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_1_multiple * 7},
		{.name = "sensor2", .index = 20, .epoch = 60.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_2_multiple * 7},
		{.name = "sensor3", .index = 21, .epoch = 60.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_3_multiple * 7},
		{.name = "sensor1", .index = 22, .epoch = 70.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_1_multiple * 8},
		{.name = "sensor2", .index = 23, .epoch = 70.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_2_multiple * 8},
		{.name = "sensor3", .index = 24, .epoch = 70.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_3_multiple * 8},
		{.name = "sensor1", .index = 25, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_1_multiple * 9},
		{.name = "sensor2", .index = 26, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_2_multiple * 9},
		{.name = "sensor3", .index = 27, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = sensor_3_multiple * 9},
	};

	SECTION("Sanity check created data is as expected") {
		SensorData returned(expected.size() + 1);
		struct libowl_filter filter {};
		REQUIRE(libowl_filter_index(&filter, LIBOWL_OP_GREATER_EQUAL, 1) == 0);
		REQUIRE(libowl_read(owl, nullptr, 0, &filter, 1, returned.data.data(), returned.data.size())
				== static_cast<int>(expected.size()));
		REQUIRE(std::equal(expected.begin(), expected.end(), returned.data.begin()));
	}

	SECTION("interval") {
		std::array<struct libowl_filter, 2> filters;
		REQUIRE(libowl_filter_epoch(&filters[0], LIBOWL_OP_GREATER_EQUAL, 0.0) == 0);
		REQUIRE(libowl_filter_epoch(&filters[1], LIBOWL_OP_LESS_EQUAL, 80.0) == 0);
		std::array<struct libowl_option, 2> options;
		REQUIRE(libowl_option_interval(&options[0], 30.0) == 0);

		SECTION("Average") {
			const std::vector<struct libowl_sensor_data> avg_expected = {
				{.name = "sensor1", .index = 7, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = 20},
				{.name = "sensor2", .index = 8, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = 200},
				{.name = "sensor3", .index = 9, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = 2000},
				{.name = "sensor1", .index = 16, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = 50},
				{.name = "sensor2", .index = 17, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = 500},
				{.name = "sensor3", .index = 18, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = 5000},
				{.name = "sensor1", .index = 25, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 80},
				{.name = "sensor2", .index = 26, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 800},
				{.name = "sensor3", .index = 27, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 8000},
			};

			REQUIRE(libowl_option_avg(&options[1]) == 0);
			SensorData returned(avg_expected.size() + 1);
			REQUIRE(libowl_read(owl, options.data(), options.size(), filters.data(), filters.size(), returned.data.data(), returned.data.size())
					== static_cast<int>(avg_expected.size()));
			REQUIRE(std::equal(avg_expected.begin(), avg_expected.end(), returned.data.begin()));
		}

		SECTION("Max") {
			const std::vector<struct libowl_sensor_data> max_expected = {
				{.name = "sensor1", .index = 7, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = 30},
				{.name = "sensor2", .index = 8, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = 300},
				{.name = "sensor3", .index = 9, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = 3000},
				{.name = "sensor1", .index = 16, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = 60},
				{.name = "sensor2", .index = 17, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = 600},
				{.name = "sensor3", .index = 18, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = 6000},
				{.name = "sensor1", .index = 25, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 90},
				{.name = "sensor2", .index = 26, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 900},
				{.name = "sensor3", .index = 27, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 9000},
			};

			REQUIRE(libowl_option_max(&options[1]) == 0);
			SensorData returned(max_expected.size() + 1);
			REQUIRE(libowl_read(owl, options.data(), options.size(), filters.data(), filters.size(), returned.data.data(), returned.data.size())
					== static_cast<int>(max_expected.size()));
			REQUIRE(std::equal(max_expected.begin(), max_expected.end(), returned.data.begin()));
		}

		SECTION("Min") {
			const std::vector<struct libowl_sensor_data> min_expected = {
				{.name = "sensor1", .index = 7, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = 10},
				{.name = "sensor2", .index = 8, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = 100},
				{.name = "sensor3", .index = 9, .epoch = 20.0, .type = LIBOWL_SENSOR_TEMP, .value = 1000},
				{.name = "sensor1", .index = 16, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = 40},
				{.name = "sensor2", .index = 17, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = 400},
				{.name = "sensor3", .index = 18, .epoch = 50.0, .type = LIBOWL_SENSOR_TEMP, .value = 4000},
				{.name = "sensor1", .index = 25, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 70},
				{.name = "sensor2", .index = 26, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 700},
				{.name = "sensor3", .index = 27, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 7000},
			};

			REQUIRE(libowl_option_min(&options[1]) == 0);
			SensorData returned(min_expected.size() + 1);
			REQUIRE(libowl_read(owl, options.data(), options.size(), filters.data(), filters.size(), returned.data.data(), returned.data.size())
					== static_cast<int>(min_expected.size()));
			REQUIRE(std::equal(min_expected.begin(), min_expected.end(), returned.data.begin()));
		}
	}

	SECTION("Total") {
		std::array<struct libowl_filter, 2> filters;
		REQUIRE(libowl_filter_epoch(&filters[0], LIBOWL_OP_GREATER_EQUAL, 0.0) == 0);
		REQUIRE(libowl_filter_epoch(&filters[1], LIBOWL_OP_LESS_EQUAL, 80.0) == 0);
		std::array<struct libowl_option, 1> options;

		SECTION("Average") {
			const std::vector<struct libowl_sensor_data> avg_expected = {
				{.name = "sensor1", .index = 25, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 50},
				{.name = "sensor2", .index = 26, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 500},
				{.name = "sensor3", .index = 27, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 5000},
			};
			REQUIRE(libowl_option_avg(&options[0]) == 0);
			SensorData returned(avg_expected.size() + 1);
			REQUIRE(libowl_read(owl, options.data(), options.size(), filters.data(), filters.size(), returned.data.data(), returned.data.size())
					== static_cast<int>(avg_expected.size()));
			REQUIRE(std::equal(avg_expected.begin(), avg_expected.end(), returned.data.begin()));
		}

		SECTION("Max") {
			const std::vector<struct libowl_sensor_data> max_expected = {
				{.name = "sensor1", .index = 25, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 90},
				{.name = "sensor2", .index = 26, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 900},
				{.name = "sensor3", .index = 27, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 9000},
			};
			REQUIRE(libowl_option_max(&options[0]) == 0);
			SensorData returned(max_expected.size() + 1);
			REQUIRE(libowl_read(owl, options.data(), options.size(), filters.data(), filters.size(), returned.data.data(), returned.data.size())
					== static_cast<int>(max_expected.size()));
			REQUIRE(std::equal(max_expected.begin(), max_expected.end(), returned.data.begin()));
		}

		SECTION("Min") {
			const std::vector<struct libowl_sensor_data> min_expected = {
				{.name = "sensor1", .index = 25, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 10},
				{.name = "sensor2", .index = 26, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 100},
				{.name = "sensor3", .index = 27, .epoch = 80.0, .type = LIBOWL_SENSOR_TEMP, .value = 1000},
			};
			REQUIRE(libowl_option_min(&options[0]) == 0);
			SensorData returned(min_expected.size() + 1);
			REQUIRE(libowl_read(owl, options.data(), options.size(), filters.data(), filters.size(), returned.data.data(), returned.data.size())
					== static_cast<int>(min_expected.size()));
			REQUIRE(std::equal(min_expected.begin(), min_expected.end(), returned.data.begin()));
		}
	}
}

TEST_CASE("impl-datapoints") {
	struct libowl_impl_dp idp {};
	auto at_exit = std::unique_ptr<struct libowl_impl_dp, Deleter>(&idp);

	SECTION("Empty") {
		REQUIRE(libowl_impl_dp_sensors_size(&idp) == 0);
	}
	SECTION("Single sensor single value") {
		REQUIRE(libowl_impl_dp_push(&idp, "test1", LIBOWL_SENSOR_TEMP, 1.0, 100) == 0);

		/* confirm value */
		REQUIRE(libowl_impl_dp_sensors_size(&idp) == 1);
		REQUIRE(libowl_impl_dp_values_size(&idp, 0) == 1);
		const struct libowl_impl_dp_value *dpv = nullptr;
		dpv = libowl_impl_dp_value(&idp, 0, 0);
		REQUIRE(dpv->value == 100);

		/* trim -- within range -- no change */
		libowl_impl_dp_trim(&idp, 0.0, 1);

		/* confirm value */
		REQUIRE(libowl_impl_dp_sensors_size(&idp) == 1);
		REQUIRE(libowl_impl_dp_values_size(&idp, 0) == 1);
		dpv = libowl_impl_dp_value(&idp, 0, 0);
		REQUIRE(dpv->value == 100);

		/* trim -- out of range -- no values */
		libowl_impl_dp_trim(&idp, 2.0, 1);
		REQUIRE(libowl_impl_dp_sensors_size(&idp) == 0);
	}

	SECTION("Two sensors single value") {
		REQUIRE(libowl_impl_dp_push(&idp, "test1", LIBOWL_SENSOR_TEMP, 1.0, 100) == 0);
		REQUIRE(libowl_impl_dp_push(&idp, "test2", LIBOWL_SENSOR_TEMP, 2.0, 200) == 0);

		/* confirm value */
		REQUIRE(libowl_impl_dp_sensors_size(&idp) == 2);
		REQUIRE(libowl_impl_dp_values_size(&idp, 0) == 1);
		REQUIRE(libowl_impl_dp_values_size(&idp, 1) == 1);
		const struct libowl_impl_dp_value *dpv = nullptr;
		dpv = libowl_impl_dp_value(&idp, 0, 0);
		REQUIRE(dpv->value == 100);
		dpv = libowl_impl_dp_value(&idp, 1, 0);
		REQUIRE(dpv->value == 200);

		/* trim -- within range -- no change */
		libowl_impl_dp_trim(&idp, 0.0, 1);

		/* confirm value */
		REQUIRE(libowl_impl_dp_sensors_size(&idp) == 2);
		REQUIRE(libowl_impl_dp_values_size(&idp, 0) == 1);
		REQUIRE(libowl_impl_dp_values_size(&idp, 1) == 1);
		dpv = libowl_impl_dp_value(&idp, 0, 0);
		REQUIRE(dpv->value == 100);
		dpv = libowl_impl_dp_value(&idp, 1, 0);
		REQUIRE(dpv->value == 200);

		SECTION("trim both") {
			/* trim -- out of range -- no values */
			libowl_impl_dp_trim(&idp, 3.0, 1);
			REQUIRE(libowl_impl_dp_sensors_size(&idp) == 0);
		}

		SECTION("trim last") {
			/* trim -- remove first -- no values */
			libowl_impl_dp_trim(&idp, 2.0, 1);
			REQUIRE(libowl_impl_dp_sensors_size(&idp) == 1);

			/* confirm value */
			REQUIRE(libowl_impl_dp_sensors_size(&idp) == 1);
			REQUIRE(libowl_impl_dp_values_size(&idp, 0) == 1);
			dpv = libowl_impl_dp_value(&idp, 0, 0);
			REQUIRE(dpv->value == 200);
		}
	}
}

void dump_sensors(const struct libowl_datapoints* dp)
{
	for (int i = 0; i < libowl_datapoints_sensor(dp, -1, nullptr, nullptr); ++i) {
		char *name = nullptr;
		int type = 0;
		libowl_datapoints_sensor(dp, i, &name, &type);
		for (int j = 0; j < libowl_datapoints_sensor_data(dp, i, -1, nullptr, nullptr); ++j) {
			int value = 0;
			double epoch = 0.0;
			libowl_datapoints_sensor_data(dp, i, j, &value, &epoch);
			printf("%s [%d]: %f - %d\n", name, type, epoch, value);
		}
	}
}

TEST_CASE("datapoints") {
	/* Prepare database with separate entries, use our monotonic with second resolution to avoid issues with epoch double precision */
	struct libowl *owl = nullptr;
	REQUIRE(libowl_open(&owl, "file::memory:?cache=shared", LIBOWL_OPEN_WRITE | LIBOWL_TIMESTAMP_MONOTONIC) == 0);
	auto at_exit = std::unique_ptr<struct libowl, Deleter>(owl);
	libowl_set_loglevel(owl, LIBOWL_LOGLEVEL_ERROR);
	int seconds = 0;
	REQUIRE(libowl_set_monotonic(owl, monotonic_s, &seconds) == 0);

	const int sensor_1_multiple = 10;
	const int sensor_2_multiple = 100;
	const int sensor_3_multiple = 1000;

	int sensor1_value = 0;
	int sensor2_value = 0;
	int sensor3_value = 0;
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "sensor1", 0, &dummy_ops, 0, &sensor1_value) == 0);
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "sensor2", 0, &dummy_ops, 0, &sensor2_value) == 0);
	REQUIRE(libowl_add_sensor(owl, LIBOWL_SENSOR_TEMP, "sensor3", 0, &dummy_ops, 0, &sensor3_value) == 0);

	/* Add 30 readings for each sensor at 10 second intervals */
	for (int i = 0; i < 30; ++i) {
		REQUIRE(libowl_update(owl) == 3);
		seconds += 10;
		sensor1_value += sensor_1_multiple;
		sensor2_value += sensor_2_multiple;
		sensor3_value += sensor_3_multiple;
	}

	SECTION("Single sensor1") {
		/* Read 2 points at 30 second intervals */
		struct libowl_filter filters[1];
		REQUIRE(libowl_filter_name(&filters[0], LIBOWL_OP_IN, "sensor1") == 0);
		struct libowl_datapoints *dp = nullptr;
		REQUIRE(libowl_datapoints_create(owl, &dp, 2, 30.0, filters, 1, LIBOWL_DATAPOINTS_MAX) == 0);
		auto dp_at_exit = std::unique_ptr<struct libowl_datapoints, Deleter>(dp);

		seconds = 50;
		for (int i = 0; i < 9; ++i) {
			REQUIRE(libowl_datapoints_update(dp) == 1);
			seconds += 30;

			/* single sensor*/
			REQUIRE(libowl_datapoints_sensor(dp, -1, nullptr, nullptr) == 1);
			char* name = nullptr;
			int type = -1;
			REQUIRE(libowl_datapoints_sensor(dp, 0, &name, &type) == 0);
			REQUIRE(strcmp(name, "sensor1") == 0);
			REQUIRE(type == LIBOWL_SENSOR_TEMP);
			/* two values */
			REQUIRE(libowl_datapoints_sensor_data(dp, 0, -1, nullptr, nullptr) == 2);
			double epoch_first = 0.0;
			int value_first = 0;
			double epoch_second = 0.0;
			int value_second = 0;
			REQUIRE(libowl_datapoints_sensor_data(dp, 0, 0, &value_first, &epoch_first) == 0);
			REQUIRE(libowl_datapoints_sensor_data(dp, 0, 1, &value_second, &epoch_second) == 0);

			/* validate results are as expected */
			REQUIRE(value_first == (i * 3 * sensor_1_multiple) + (2 * sensor_1_multiple));
			//REQUIRE(epoch_first == )
			REQUIRE(value_second == ((i + 1) * 3 * sensor_1_multiple) + (2 * sensor_1_multiple));

			/* get range */
			double start = 0.0;
			double end = 0.0;
			REQUIRE(libowl_datapoints_range(dp, 0, &start, &end) == 0);
			REQUIRE((long) start == (i * 3 * sensor_1_multiple) + (2 * sensor_1_multiple));
			REQUIRE((long) end == ((i + 1) * 3 * sensor_1_multiple) + (2 * sensor_1_multiple));
		}

	}
}
