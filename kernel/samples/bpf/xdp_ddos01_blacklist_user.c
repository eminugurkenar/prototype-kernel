/* Copyright(c) 2017 Jesper Dangaard Brouer, Red Hat, Inc.
 */
static const char *__doc__=
 " XDP example: DDoS protection via IPv4 blacklist";

#include <linux/bpf.h>

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <sys/resource.h>
#include <getopt.h>
#include <net/if.h>

#include <arpa/inet.h>

#include "bpf_load.h"
#include "bpf_util.h"
#include "libbpf.h"

static char ifname_buf[IF_NAMESIZE];
static char *ifname = NULL;
static int ifindex = -1;
static int verbose = 1;

/* Export eBPF map for IPv4 blacklist as a file
 * Gotcha need to mount:
 *   mount -t bpf bpf /sys/fs/bpf/
 */
static const char *file_blacklist = "/sys/fs/bpf/ddos_blacklist";

static void int_exit(int sig)
{
	fprintf(stderr, "Interrupted: Removing XDP program on"
		" ifindex:%d device:%s\n",
		ifindex, ifname);
	if (ifindex > -1)
		set_link_xdp_fd(ifindex, -1);
	exit(0);
}

static const struct option long_options[] = {
	{"help",	no_argument,		NULL, 'h' },
	{"dev",		required_argument,	NULL, 'd' },
	{0, 0, NULL,  0 }
};

/* Exit return codes */
#define	EXIT_OK			0
#define EXIT_FAIL		1
#define EXIT_FAIL_OPTION	2
#define EXIT_FAIL_XDP		3
#define EXIT_FAIL_KEY_UPDATE	4
#define EXIT_FAIL_MAP		5
#define EXIT_FAIL_IP		102

static void usage(char *argv[])
{
	int i;
	printf("\nDOCUMENTATION:\n%s\n", __doc__);
	printf("\n");
	printf(" Usage: %s (options-see-below)\n",
	       argv[0]);
	printf(" Listing options:\n");
	for (i = 0; long_options[i].name != 0; i++) {
		printf(" --%-12s", long_options[i].name);
		if (long_options[i].flag != NULL)
			printf(" flag (internal value:%d)",
			       *long_options[i].flag);
		else
			printf(" short-option: -%c",
			       long_options[i].val);
		printf("\n");
	}
	printf("\n");
}

static void stats_print_headers(void)
{
	static unsigned int i;
#define DEBUG 1
#ifdef  DEBUG
	{
	int debug_notice_interval = 3;
	char msg[] =
		"\nDebug output available via:\n"
		" sudo cat /sys/kernel/debug/tracing/trace_pipe\n\n";
	printf(msg, debug_notice_interval);
	}
#endif
	i++;
	printf("Stats: %d\n", i);
}

struct stats_key {
	__u32 key;
	__u64 value_sum;
};

static void stats_print(struct stats_key *record)
{
	__u64 count;
	__u32 key;

	key   = record->key;
	count = record->value_sum;
	if (count)
		printf("Key: IP-src-raw:0x%X count:%llu\n", key, count);
}
static bool stats_collect(struct stats_key *record, __u32 key)
{
	unsigned int nr_cpus = bpf_num_possible_cpus();
	__u64 values[nr_cpus];
	__u64 sum = 0;
	int i;

	if ((bpf_map_lookup_elem(map_fd[0], &key, values)) != 0) {
		printf("DEBUG: bpf_map_lookup_elem failed\n");
		return false;
	}

	/* Sum values from each CPU */
	for (i = 0; i < nr_cpus; i++) {
		sum += values[i];
	}

	record->value_sum = sum;
	record->key = key;
	return true;
}

static void stats_poll(void)
{
	struct stats_key record;
	__u32 key = 0, next_key;

	/* clear screen */
	printf("\033[2J");
	stats_print_headers();

	while (bpf_map_get_next_key(map_fd[0], &key, &next_key) == 0) {

		memset(&record, 0, sizeof(record));
		if (stats_collect(&record, next_key))
			stats_print(&record);

		key = next_key;
	}
}

static void blacklist_add(char *ip_string)
{
	__u64 value = 0;
	__u32 key;
	int res;

	/* Convert IP-string into 32-bit network byte-order value */
	res = inet_pton(AF_INET, ip_string, &key);
	if (res <= 0) {
		if (res == 0)
			fprintf(stderr,
			"ERROR: IPv4 \"%s\" not in presentation format\n",
				ip_string);
		else
			perror("inet_pton");
		exit(EXIT_FAIL_IP);
	}

	res = bpf_map_update_elem(map_fd[0], &key, &value, BPF_NOEXIST);
	if (res != 0) { /* 0 == success */

		printf("%s() IP:%s key:0x%X errno(%d/%s)",
		       __func__, ip_string, key, errno, strerror(errno));

		if (errno == 17) {
			printf(": Already in blacklist\n");
			return;
		}
		printf("\n");
		exit(EXIT_FAIL_KEY_UPDATE);
	}
	if (verbose)
		printf("%s() IP:%s key:0x%X\n", __func__, ip_string, key);
}

int main(int argc, char **argv)
{
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	char filename[256];
	int longindex = 0;
	int interval = 2;
	int opt;

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);

	/* Parse commands line args */
	while ((opt = getopt_long(argc, argv, "hi:",
				  long_options, &longindex)) != -1) {
		switch (opt) {
		case 'd':
			if (strlen(optarg) >= IF_NAMESIZE) {
				printf("ERR: --dev name too long\n");
				goto error;
			}
			ifname = (char *)&ifname_buf;
			strncpy(ifname, optarg, IF_NAMESIZE);
			ifindex = if_nametoindex(ifname);
			if (ifindex == 0) {
				printf("ERR: --dev name unknown err(%d):%s\n",
				       errno, strerror(errno));
				goto error;
			}
			if (verbose)
				printf("Device:%s have ifindex:%d\n",
				       ifname, ifindex);
			break;
		case 'h':
		error:
		default:
			usage(argv);
			return EXIT_FAIL_OPTION;
		}
	}
	/* Required options */
	if (ifindex == -1) {
		printf("ERR: required option --dev missing");
		usage(argv);
		return EXIT_FAIL_OPTION;
	}

	/* Increase resource limits */
	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
		perror("setrlimit(RLIMIT_MEMLOCK, RLIM_INFINITY)");
		return 1;
	}

	if (load_bpf_file(filename)) {
		printf("%s", bpf_log_buf);
		return 1;
	}

	if (!prog_fd[0]) {
		printf("load_bpf_file: %s\n", strerror(errno));
		return 1;
	}

	/* Export map as a file */
	if (bpf_obj_pin(map_fd[0], file_blacklist) != 0) {
		if (errno == 17) {
			/* File exists, remove it as this bpf XDP
			 * program force-fully overwrite/swap existing
			 * XDP prog.
			 */
			printf("Del previous map file: %s\n", file_blacklist);
			if (unlink(file_blacklist) < 0) {
				printf("ERR: cannot cleanup old map"
				       "file:%s err(%d):%s\n",
				       file_blacklist, errno, strerror(errno));
				exit(EXIT_FAIL_MAP);
			}
			/* FIXME: shouldn't we let an existing
			 * blacklist map "survive", and feed it to the
			 * eBPF program?
			 */
		} else {
			printf("ERR: Cannot pin map file:%s err(%d):%s\n",
			       file_blacklist, errno, strerror(errno));
			return EXIT_FAIL_MAP;
		}
	}
	printf("Blacklist exported to file: %s\n", file_blacklist);

	/* Remove XDP program when program is interrupted */
	signal(SIGINT, int_exit);

	if (set_link_xdp_fd(ifindex, prog_fd[0]) < 0) {
		printf("link set xdp fd failed\n");
		return EXIT_FAIL_XDP;
	}

	blacklist_add("192.2.1.3");
	blacklist_add("192.2.1.3");
	sleep(10);
	blacklist_add("198.18.50.3");

	while (1) {
		stats_poll();
		sleep(interval);
	}

	return EXIT_OK;
}