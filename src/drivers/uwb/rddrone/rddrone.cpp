/****************************************************************************
 *
 *   Copyright (c) 2019 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "rddrone.h"
#include <px4_platform_common/log.h>
#include <px4_platform_common/getopt.h>
#include <px4_platform_common/cli.h>
#include <errno.h>
#include <fcntl.h>
#include <systemlib/err.h>
#include <drivers/drv_hrt.h>

// Timeout between bytes. If there is more time than this between bytes, then this driver assumes
// that it is the boundary between messages.
// See RDDrone::run() for more detailed explanation.
#define BYTE_TIMEOUT_US 5000

// Amount of time to wait for a new message. If more time than this passes between messages, then this
// driver assumes that the RDDrone module is disconnected.
// (Right now it does not do anything about this)
#define MESSAGE_TIMEOUT_S 10  //wait 10 seconds.
#define MESSAGE_TIMEOUT_US 1

// The default baudrate of the RDDrone module before configuration
#define DEFAULT_BAUD B115200

extern "C" __EXPORT int rddrone_main(int argc, char *argv[]);

RDDrone::RDDrone(const char *device_name, speed_t baudrate):
	_read_count_perf(perf_alloc(PC_COUNT, "rddrone_count")),
	_read_err_perf(perf_alloc(PC_COUNT, "rddrone_err"))
{
	// start serial port
	_uart = open(device_name, O_RDWR | O_NOCTTY);

	if (_uart < 0) { err(1, "could not open %s", device_name); }

	int ret = 0;
	struct termios uart_config {};
	ret = tcgetattr(_uart, &uart_config);

	if (ret < 0) { err(1, "failed to get attr"); }

	uart_config.c_oflag &= ~ONLCR; // no CR for every LF
	ret = cfsetispeed(&uart_config, baudrate);

	if (ret < 0) { err(1, "failed to set input speed"); }

	ret = cfsetospeed(&uart_config, baudrate);

	if (ret < 0) { err(1, "failed to set output speed"); }

	ret = tcsetattr(_uart, TCSANOW, &uart_config);

	if (ret < 0) { err(1, "failed to set attr"); }

}

RDDrone::~RDDrone()
{
	perf_free(_read_err_perf);
	perf_free(_read_count_perf);

	close(_uart);
}

void RDDrone::run()
{

	/* Grid Survey */

	uint8_t *grid_buffer = (uint8_t *) &_grid_survey_msg;
	bool grid_found = 0;

	while (grid_found == 0) {

		int written = write(_uart, CMD_GRID_SURVEY, sizeof(CMD_GRID_SURVEY));

		if (written < (int) sizeof(CMD_GRID_SURVEY)) {
			PX4_ERR("Only wrote %d bytes out of %d.", written, (int) sizeof(CMD_GRID_SURVEY));
		}

		/*Do Grid Survey:*/

		FD_ZERO(&_uart_set);
		FD_SET(_uart, &_uart_set);
		_uart_timeout.tv_sec = MESSAGE_TIMEOUT_S;
		_uart_timeout.tv_usec = MESSAGE_TIMEOUT_US;

		size_t grid_buffer_location = 0;
		// Messages are only delimited by time. There is a chance that this driver starts up in the middle
		// of a message, with no way to know this other than time. There is also always the possibility of
		// transmission errors causing a dropped byte.
		// Here is the process for dealing with that:
		//  - Wait up to 1 second to start receiving a message
		//  - Once receiving a message, keep going until EITHER:
		//    - There is too large of a gap between bytes (Currently set to 5ms).
		//      This means the message is incomplete. Throw it out and start over.
		//    - 51 bytes are received (the size of the whole message).
		// TODO add second message

		while (grid_buffer_location < sizeof(_grid_survey_msg)
		       && select(_uart + 1, &_uart_set, nullptr, nullptr, &_uart_timeout) > 0) {

			int bytes_read = read(_uart, &grid_buffer[grid_buffer_location], sizeof(_grid_survey_msg) - grid_buffer_location);

			if (bytes_read > 0) {
				grid_buffer_location += bytes_read;

			} else {
				break;
			}

			FD_ZERO(&_uart_set);
			FD_SET(_uart, &_uart_set);
			_uart_timeout.tv_sec = 0;
			// Setting this timeout too high (> 37ms) will cause problems because the next message will start
			//  coming in, and overlap with the current message.
			// Setting this timeout too low (< 1ms) will cause problems because there is some delay between
			//  the individual bytes of a message, and a too-short timeout will cause the message to be truncated.
			// The current value of 5ms was found experimentally to never cut off a message prematurely.
			// Strictly speaking, there are no downsides to setting this timeout as high as possible (Just under 37ms),
			// because if this process is waiting, it means that the last message was incomplete, so there is no current
			// data waiting to be published. But we would rather set this timeout lower in case the RDDrone board is
			// updated to publish data faster.
			_uart_timeout.tv_usec = BYTE_TIMEOUT_US;
		}

		// All of the following criteria must be met for the message to be acceptable:
		//  - Size of message == sizeof(grid_msg_t) (163)
		//  - status == 0x00
		//  - Stop Byte == 0x1b
		//  - Values of all 3 position measurements are reasonable
		//      (If one or more anchors is missed, then position might be an unreasonably large number.)
		grid_found = (grid_buffer_location == sizeof(grid_msg_t) && _grid_survey_msg.stop == 0x1b);
		perf_count(_read_count_perf);

	}

	_uwb_grid.timestamp = hrt_absolute_time();
	_attitude_sub.update(&_vehicle_attitude);

	memcpy(&_uwb_grid.grid_uuid, &_grid_survey_msg.grid_uuid, sizeof(_uwb_grid.grid_uuid));
	_uwb_grid.initator_time = _grid_survey_msg.initator_time;
	_uwb_grid.anchor_nr = _grid_survey_msg.anchor_nr;


	memcpy(&_uwb_grid.gps, &_grid_survey_msg.gps, sizeof(gps_pos_t));
	memcpy(&_uwb_grid.target_pos, &_grid_survey_msg.target_pos, sizeof(position_t));

	//for (int i = 0; i < MAX_ANCHORS; i++) {
	memcpy(&_uwb_grid.anchor_pos_0, &_grid_survey_msg.anchor_pos[0], sizeof(position_t)); //how can i do this with a Loop?
	memcpy(&_uwb_grid.anchor_pos_1, &_grid_survey_msg.anchor_pos[1],
	       sizeof(position_t)); //the Source Data is Structured but the Tartget Data not
	memcpy(&_uwb_grid.anchor_pos_2, &_grid_survey_msg.anchor_pos[2], sizeof(position_t));
	memcpy(&_uwb_grid.anchor_pos_3, &_grid_survey_msg.anchor_pos[3], sizeof(position_t));
	memcpy(&_uwb_grid.anchor_pos_4, &_grid_survey_msg.anchor_pos[4], sizeof(position_t));
	memcpy(&_uwb_grid.anchor_pos_5, &_grid_survey_msg.anchor_pos[5], sizeof(position_t));
	memcpy(&_uwb_grid.anchor_pos_6, &_grid_survey_msg.anchor_pos[6], sizeof(position_t));
	memcpy(&_uwb_grid.anchor_pos_7, &_grid_survey_msg.anchor_pos[7], sizeof(position_t));
	memcpy(&_uwb_grid.anchor_pos_8, &_grid_survey_msg.anchor_pos[8], sizeof(position_t));
	//}


	_uwb_grid_pub.publish(_uwb_grid);

	printf("GRID FOUND.\t\n");



	sleep(1);

	// After Grid Survey the Drone Starts to Range

	/* Ranging */
	int written = write(_uart, CMD_DISTANCE_RESULT, sizeof(CMD_DISTANCE_RESULT));

	if (written < (int) sizeof(CMD_DISTANCE_RESULT)) {
		PX4_ERR("Only wrote %d bytes out of %d.", written, (int) sizeof(CMD_DISTANCE_RESULT));
	}

	uint8_t *buffer = (uint8_t *) &_distance_result_msg;

	while (!should_exit()) {

		FD_ZERO(&_uart_set);
		FD_SET(_uart, &_uart_set);
		_uart_timeout.tv_sec = MESSAGE_TIMEOUT_S ;
		_uart_timeout.tv_usec = MESSAGE_TIMEOUT_US;

		size_t buffer_location = 0;

		// Messages are only delimited by time. There is a chance that this driver starts up in the middle
		// of a message, with no way to know this other than time. There is also always the possibility of
		// transmission errors causing a dropped byte.
		// Here is the process for dealing with that:
		//  - Wait up to 1 second to start receiving a message
		//  - Once receiving a message, keep going until EITHER:
		//    - There is too large of a gap between bytes (Currently set to 5ms).
		//      This means the message is incomplete. Throw it out and start over.
		//    - 51 bytes are received (the size of the whole message).
		// TODO add second message
		while (buffer_location < sizeof(_distance_result_msg)
		       && select(_uart + 1, &_uart_set, nullptr, nullptr, &_uart_timeout) > 0) {

			int bytes_read = read(_uart, &buffer[buffer_location], sizeof(_distance_result_msg) - buffer_location);

			if (bytes_read > 0) {
				buffer_location += bytes_read;

			} else {
				break;
			}

			FD_ZERO(&_uart_set);
			FD_SET(_uart, &_uart_set);
			_uart_timeout.tv_sec = 0;
			// Setting this timeout too high (> 37ms) will cause problems because the next message will start
			//  coming in, and overlap with the current message.
			// Setting this timeout too low (< 1ms) will cause problems because there is some delay between
			//  the individual bytes of a message, and a too-short timeout will cause the message to be truncated.
			// The current value of 5ms was found experimentally to never cut off a message prematurely.
			// Strictly speaking, there are no downsides to setting this timeout as high as possible (Just under 37ms),
			// because if this process is waiting, it means that the last message was incomplete, so there is no current
			// data waiting to be published. But we would rather set this timeout lower in case the RDDrone board is
			// updated to publish data faster.
			_uart_timeout.tv_usec = BYTE_TIMEOUT_US;
		}

		perf_count(_read_count_perf);



		// All of the following criteria must be met for the message to be acceptable:
		//  - Size of message == sizeof(distance_msg_t) (51 bytes)
		//  - status == 0x00
		//  - Values of all 3 position measurements are reasonable
		//      (If one or more anchors is missed, then position might be an unreasonably large number.)
		bool ok = (buffer_location == sizeof(distance_msg_t) && _distance_result_msg.stop == 0x1b); //||
		//(buffer_location == sizeof(grid_msg_t) && _distance_result_msg.stop == 0x1b)
		//);


		if (ok) {
			_uwb_distance.timestamp = hrt_absolute_time();

			_attitude_sub.update(&_vehicle_attitude);
			_uwb_distance.status = _distance_result_msg.status;
			_uwb_distance.counter = _distance_result_msg.counter;
			_uwb_distance.yaw_offset = _distance_result_msg.yaw_offset;
			_uwb_distance.time_offset = _distance_result_msg.time_offset;

			/*
						// The end goal of this math is to get the position relative to the landing point in the NED frame.
						// Current position, in RDDrone frame
						_current_position_rddrone = matrix::Vector3f(_distance_result_msg.pos_x, _distance_result_msg.pos_y, _distance_result_msg.pos_z);
						// Construct the rotation from the RDDrone frame to the NWU frame.
						// The RDDrone frame is just NWU, rotated by some amount about the Z (up) axis.
						// To get back to NWU, just rotate by negative this amount about Z.
						_rddrone_to_nwu = matrix::Dcmf(matrix::Eulerf(0.0f, 0.0f, -(_distance_result_msg.yaw_offset * M_PI_F / 180.0f)));
						// The actual conversion:
						//  - Subtract _landing_point to get the position relative to the landing point, in RDDrone frame
						//  - Rotate by _rddrone_to_nwu to get into the NWU frame
						//  - Rotate by _nwu_to_ned to get into the NED frame
						_current_position_ned = _nwu_to_ned * _rddrone_to_nwu * _current_position_rddrone;

						// Now the position is the vehicle relative to the landing point. We need the landing point relative to
						// the vehicle. So just negate everything.
						_uwb_distance.target_pos_x = _current_position_ned(0);
						_uwb_distance.target_pos_y = _current_position_ned(1);
						_uwb_distance.target_pos_z = _current_position_ned(2);
			*/
			for (int i = 0; i < MAX_ANCHORS; i++) {
				_uwb_distance.anchor_distance[i] = _distance_result_msg.anchor_distance[i];
			}

			_uwb_distance_pub.publish(_uwb_distance);

		} else {
			//PX4_ERR("Read %d bytes instead of %d.", (int) buffer_location, (int) sizeof(distance_msg_t));
			perf_count(_read_err_perf);

			if (buffer_location == 0) {
				PX4_WARN("UWB module is not responding.");
			}

		}
	}


	//Stop. This should not be reachable
	written = write(_uart, &CMD_STOP_RANGING, sizeof(CMD_STOP_RANGING));

	if (written < (int) sizeof(CMD_STOP_RANGING)) {
		PX4_ERR("Only wrote %d bytes out of %d.", written, (int) sizeof(CMD_STOP_RANGING));
	}

}

int RDDrone::custom_command(int argc, char *argv[])
{
	return print_usage("Unrecognized command.");
}

int RDDrone::print_usage(const char *reason)
{
	if (reason) {
		printf("%s\n\n", reason);
	}

	PRINT_MODULE_USAGE_NAME("uwb", "driver");
	PRINT_MODULE_DESCRIPTION(R"DESC_STR(
### Description

Driver for NXP RDDrone UWB positioning system. This driver publishes a `uwb_distance` message
whenever the RDDrone has a position measurement available.

### Example

Start the driver with a given device:

$ uwb start -d /dev/ttyS2
	)DESC_STR");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_PARAM_STRING('d', nullptr, "<file:dev>", "Name of device for serial communication with UWB", false);
	PRINT_MODULE_USAGE_PARAM_STRING('b', nullptr, "<int>", "Baudrate for serial communication", false);
	PRINT_MODULE_USAGE_COMMAND("stop");
	PRINT_MODULE_USAGE_COMMAND("status");
	return 0;
}

int RDDrone::task_spawn(int argc, char *argv[])
{
	int task_id = px4_task_spawn_cmd(
			      "uwb_driver",
			      SCHED_DEFAULT,
			      SCHED_PRIORITY_DEFAULT,
			      2048,
			      &run_trampoline,
			      argv
		      );

	if (task_id < 0) {
		return -errno;

	} else {
		_task_id = task_id;
		return 0;
	}
}

speed_t int_to_speed(int baud)
{
	switch (baud) {
	case 9600:
		return B9600;

	case 19200:
		return B19200;

	case 38400:
		return B38400;

	case 57600:
		return B57600;

	case 115200:
		return B115200;

	default:
		return DEFAULT_BAUD;
	}
}

RDDrone *RDDrone::instantiate(int argc, char *argv[])
{
	int ch;
	int option_index = 1;
	const char *option_arg;
	const char *device_name = nullptr;
	bool error_flag = false;
	int baudrate = 0;

	while ((ch = px4_getopt(argc, argv, "d:b:", &option_index, &option_arg)) != EOF) {
		switch (ch) {
		case 'd':
			device_name = option_arg;
			break;

		case 'b':
			px4_get_parameter_value(option_arg, baudrate);
			break;

		default:
			PX4_WARN("Unrecognized flag: %c", ch);
			error_flag = true;
			break;
		}
	}

	if (!error_flag && device_name == nullptr) {
		print_usage("Device name not provided.");
		error_flag = true;
	}

	if (!error_flag && baudrate == 0) {
		print_usage("Baudrate not provided.");
		error_flag = true;
	}

	if (error_flag) {
		PX4_WARN("Failed to start UWB driver.");
		return nullptr;

	} else {
		PX4_INFO("Constructing RDDrone. Device: %s", device_name);
		return new RDDrone(device_name, int_to_speed(baudrate));
	}
}

int rddrone_main(int argc, char *argv[])
{
	return RDDrone::main(argc, argv);
}
