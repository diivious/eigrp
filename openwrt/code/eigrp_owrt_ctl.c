// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenWrt read-only operational client. Copyright (C) 2026 Donnie V. Savage */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

#define EIGRP_OWRT_SOCKET_DEFAULT "/var/run/openeigrpd.sock"

int main(int argc, char **argv)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	struct timeval timeout = {.tv_sec = 4};
	const char *command = NULL, *path = EIGRP_OWRT_SOCKET_DEFAULT;
	char buffer[2048];
	ssize_t n;
	int fd, i;
	for (i=1; i<argc; i++) {
		if (!strcmp(argv[i], "--socket") && i+1<argc) path = argv[++i];
		else if (!strcmp(argv[i], "--help")) {
			puts("Usage: openeigrpctl [--socket PATH] status|interfaces|neighbors|topology|traffic|events");
			return 0;
		} else if (!command) command = argv[i];
		else command = NULL;
	}
	if (!command ||
	    (strcmp(command, "status") && strcmp(command, "interfaces") &&
	     strcmp(command, "neighbors") && strcmp(command, "topology") &&
	     strcmp(command, "traffic") && strcmp(command, "events"))) {
		fprintf(stderr, "openeigrpctl: invalid command (try --help)\n");
		return 2;
	}
	if (strlen(path) >= sizeof(address.sun_path)) return 2;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {perror("openeigrpctl: socket"); return 1;}
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		perror("openeigrpctl: connect"); close(fd); return 1;
	}
	if (send(fd, command, strlen(command), MSG_NOSIGNAL) != (ssize_t)strlen(command) ||
	    send(fd, "\n", 1, MSG_NOSIGNAL) != 1) {
		perror("openeigrpctl: send"); close(fd); return 1;
	}
	(void)shutdown(fd, SHUT_WR);
	while ((n = read(fd, buffer, sizeof(buffer))) > 0)
		if (fwrite(buffer, 1, (size_t)n, stdout) != (size_t)n) {close(fd); return 1;}
	if (n < 0) {perror("openeigrpctl: read"); close(fd); return 1;}
	close(fd);
	return 0;
}
