// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Minimal force-feedback smoke test: upload one FF_RUMBLE effect, play it for
 * one second, stop it.  Run it against the virtual DualShock 4 event node
 * while g7motion is running.
 *
 *   cc -O2 -Wall -Wextra -o build/rumble_test tests/rumble_test.c
 *   ./build/rumble_test /dev/input/eventN
 *
 * Both physical grips should vibrate for one second.
 */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int supports_rumble(int fd)
{
	unsigned long ffbit[1 + FF_MAX / (8 * sizeof(unsigned long))] = {};

	if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffbit)), ffbit) < 0)
		return -1;

	return !!(ffbit[FF_RUMBLE / (8 * sizeof(unsigned long))] &
		  (1UL << (FF_RUMBLE % (8 * sizeof(unsigned long)))));
}

int main(int argc, char **argv)
{
	struct ff_effect effect = { 0 };
	struct input_event play = { 0 };
	int fd, has_rumble;

	if (argc != 2) {
		fprintf(stderr, "usage: %s /dev/input/eventN\n", argv[0]);
		return 2;
	}

	fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	has_rumble = supports_rumble(fd);
	if (has_rumble < 0) {
		perror("EVIOCGBIT");
		close(fd);
		return 1;
	}
	if (!has_rumble) {
		fprintf(stderr, "%s does not advertise FF_RUMBLE\n", argv[1]);
		close(fd);
		return 1;
	}

	effect.type = FF_RUMBLE;
	effect.id = -1;
	effect.u.rumble.strong_magnitude = 0xffff;
	effect.u.rumble.weak_magnitude = 0xffff;
	effect.replay.length = 1000;
	effect.replay.delay = 0;

	if (ioctl(fd, EVIOCSFF, &effect) < 0) {
		perror("EVIOCSFF");
		close(fd);
		return 1;
	}

	play.type = EV_FF;
	play.code = effect.id;
	play.value = 1;
	if (write(fd, &play, sizeof(play)) != sizeof(play)) {
		perror("write play");
		close(fd);
		return 1;
	}

	printf("rumble_test: playing 1 s\n");
	usleep(1000000);

	play.value = 0;
	if (write(fd, &play, sizeof(play)) != sizeof(play))
		perror("write stop");
	(void)ioctl(fd, EVIOCRMFF, effect.id);

	close(fd);
	printf("rumble_test: OK\n");
	return 0;
}
