// SPDX-License-Identifier: BSD-2-Clause
/* Copyright (c) 2021 Facebook */
#include <argp.h>
#include <ctype.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/btf.h>
#include <linux/perf_event.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "retsnoop.h"
#include "retsnoop.skel.h"
#include "calib_feat.skel.h"
#include "ksyms.h"
#include "addr2line.h"
#include "mass_attacher.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))
#define MIN(x, y) ((x) < (y) ? (x): (y))

struct ctx {
	struct mass_attacher *att;
	struct retsnoop_bpf *skel;
	struct ksyms *ksyms;
	struct addr2line *a2l;
};

enum attach_mode {
	ATTACH_DEFAULT,
	ATTACH_KPROBE_MULTI,
	ATTACH_KPROBE_SINGLE,
	ATTACH_FENTRY,
};

enum symb_mode {
	SYMB_NONE = -1,

	SYMB_DEFAULT = 0,
	SYMB_LINEINFO = 0x1,
	SYMB_INLINES = 0x2,
};

static struct env {
	bool show_version;
	bool verbose;
	bool debug;
	bool debug_extra;
	bool bpf_logs;
	bool dry_run;
	bool emit_success_stacks;
	bool emit_full_stacks;
	bool emit_intermediate_stacks;
	enum attach_mode attach_mode;
	enum symb_mode symb_mode;
	bool use_lbr;
	long lbr_flags;
	const char *vmlinux_path;
	int pid;
	int longer_than_ms;

	char **allow_globs;
	char **deny_globs;
	char **entry_globs;
	int allow_glob_cnt;
	int deny_glob_cnt;
	int entry_glob_cnt;

	char **cu_allow_globs;
	char **cu_deny_globs;
	char **cu_entry_globs;
	int cu_allow_glob_cnt;
	int cu_deny_glob_cnt;
	int cu_entry_glob_cnt;

	int *allow_pids;
	int *deny_pids;
	int allow_pid_cnt;
	int deny_pid_cnt;

	char **allow_comms;
	char **deny_comms;
	int allow_comm_cnt;
	int deny_comm_cnt;

	int allow_error_cnt;
	bool has_error_filter;
	__u64 allow_error_mask[MAX_ERR_CNT / 64];
	__u64 deny_error_mask[MAX_ERR_CNT / 64];

	struct ctx ctx;
	int ringbuf_sz;
	int perfbuf_percpu_sz;
	int stacks_map_sz;

	int cpu_cnt;
	bool has_branch_snapshot;
	bool has_lbr;
	bool has_ringbuf;
} env = {
	.ringbuf_sz = 4 * 1024 * 1024,
	.perfbuf_percpu_sz = 256 * 1024,
	.stacks_map_sz = 1024,
};

const char *argp_program_version = "retsnoop v0.7";
const char *argp_program_bug_address = "Andrii Nakryiko <andrii@kernel.org>";
const char argp_program_doc[] =
"retsnoop tool shows kernel call stacks based on specified function filters.\n"
"\n"
"USAGE: retsnoop [-v] [-ss] [-F|-K] [-c CASE]* [-a GLOB]* [-d GLOB]* [-e GLOB]*\n";

#define OPT_FULL_STACKS 1001
#define OPT_STACKS_MAP_SIZE 1002
#define OPT_LBR 1003
#define OPT_DRY_RUN 1004

static const struct argp_option opts[] = {
	{ "verbose", 'v', "LEVEL", OPTION_ARG_OPTIONAL,
	  "Verbose output (use -vv for debug-level verbosity, -vvv for libbpf debug log)" },
	{ "version", 'V', NULL, 0,
	  "Print out retsnoop version." },
	{ "bpf-logs", 'l', NULL, 0,
	  "Emit BPF-side logs (use `sudo cat /sys/kernel/debug/tracing/trace_pipe` to read)" },
	{ "dry-run", OPT_DRY_RUN, NULL, 0,
	  "Perform a dry run (don't actually load and attach BPF programs)" },

	/* Attach mechanism specification */
	{ "kprobes-multi", 'M', NULL, 0,
	  "Use multi-attach kprobes/kretprobes, if supported; fall back to single-attach kprobes/kretprobes, otherwise" },
	{ "kprobes", 'K', NULL, 0,
	  "Use single-attach kprobes/kretprobes" },
	{ "fentries", 'F', NULL, 0,
	  "Use fentries/fexits instead of kprobes/kretprobes" },

	/* Target functions specification */
	{ "case", 'c', "CASE", 0,
	  "Use a pre-defined set of entry/allow/deny globs for a given use case (supported cases: bpf, perf)" },
	{ "entry", 'e', "GLOB", 0,
	  "Glob for entry functions that trigger error stack trace collection" },
	{ "allow", 'a', "GLOB", 0,
	  "Glob for allowed functions captured in error stack trace collection" },
	{ "deny", 'd', "GLOB", 0,
	  "Glob for denied functions ignored during error stack trace collection" },

	/* Stack filtering specification */
	{ "pid", 'p', "PID", 0,
	  "Only trace given PID. Can be specified multiple times" },
	{ "no-pid", 'P', "PID", 0,
	  "Skip tracing given PID. Can be specified multiple times" },
	{ "comm", 'n', "COMM", 0,
	  "Only trace processes with given name (COMM). Can be specified multiple times" },
	{ "no-comm", 'N', "COMM", 0,
	  "Skip tracing processes with given name (COMM). Can be specified multiple times" },
	{ "longer", 'L', "MS", 0,
	  "Only emit stacks that took at least a given amount of milliseconds" },
	{ "success-stacks", 'S', NULL, 0,
	  "Emit any stack, successful or not" },
	{ "allow-errors", 'x', "ERROR", 0, "Record stacks only with specified errors" },
	{ "deny-errors", 'X', "ERROR", 0, "Ignore stacks that have specified errors" },

	/* Misc settings */
	{ "lbr", OPT_LBR, "SPEC", OPTION_ARG_OPTIONAL,
	  "Capture and print LBR entries" },
	{ "kernel", 'k',
	  "PATH", 0, "Path to vmlinux image with DWARF information embedded" },
	{ "symbolize", 's', "LEVEL", OPTION_ARG_OPTIONAL,
	  "Set symbolization settings (-s for line info, -ss for also inline functions, -sn to disable extra symbolization). "
	  "If extra symbolization is requested, retsnoop relies on having vmlinux with DWARF available." },
	{ "intermediate-stacks", 'A', NULL, 0,
	  "Emit all partial (intermediate) stack traces" },
	{ "full-stacks", OPT_FULL_STACKS, NULL, 0,
	  "Emit non-filtered full stack traces" },
	{ "stacks-map-size", OPT_STACKS_MAP_SIZE, "SIZE", 0,
	  "Stacks map size (default 1024)" },
	{},
};

struct preset {
	const char *name;
	const char **entry_globs;
	const char **allow_globs;
	const char **deny_globs;
};

static const char *bpf_entry_globs[];
static const char *bpf_allow_globs[];
static const char *bpf_deny_globs[];

static const char *perf_entry_globs[];
static const char *perf_allow_globs[];
static const char *perf_deny_globs[];

static const struct preset presets[] = {
	{"bpf", bpf_entry_globs, bpf_allow_globs, bpf_deny_globs},
	{"perf", perf_entry_globs, perf_allow_globs, perf_deny_globs},
};

static inline __u64 now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);

	return (__u64)t.tv_sec * 1000000000 + t.tv_nsec;
}

static int append_str(char ***strs, int *cnt, const char *str)
{
	void *tmp;
	char *s;

	tmp = realloc(*strs, (*cnt + 1) * sizeof(**strs));
	if (!tmp)
		return -ENOMEM;
	*strs = tmp;

	(*strs)[*cnt] = s = strdup(str);
	if (!s)
		return -ENOMEM;

	*cnt = *cnt + 1;
	return 0;
}

static int append_str_file(char ***strs, int *cnt, const char *file)
{
	char buf[256];
	FILE *f;
	int err = 0;

	f = fopen(file, "r");
	if (!f) {
		err = -errno;
		fprintf(stderr, "Failed to open '%s': %d\n", file, err);
		return err;
	}

	while (fscanf(f, "%s", buf) == 1) {
		if (append_str(strs, cnt, buf)) {
			err = -ENOMEM;
			goto cleanup;
		}
	}

cleanup:
	fclose(f);
	return err;
}

static int append_compile_unit(struct ctx *ctx, char ***strs, int *cnt, const char *compile_unit)
{
	int err = 0;
	struct a2l_cu_resp *cu_resps = NULL;
	int resp_cnt;
	int i;

	resp_cnt = addr2line__query_symbols(ctx->a2l, compile_unit, &cu_resps);
	if (resp_cnt < 0) {
		return resp_cnt;
	}

	for (i = 0; i < resp_cnt; i++) {
		if (append_str(strs, cnt, cu_resps[i].fname)) {
			err = -ENOMEM;
			break;
		}
	}

	free(cu_resps);
	return err;
}

static int process_cu_globs()
{
	int err = 0;
	int i;

	for (i = 0; i < env.cu_allow_glob_cnt; i++) {
		err = append_compile_unit(&env.ctx, &env.allow_globs, &env.allow_glob_cnt, env.cu_allow_globs[i]);
		if (err < 0) {
			return err;
		}
	}

	for (i = 0; i < env.cu_deny_glob_cnt; i++) {
		err = append_compile_unit(&env.ctx, &env.deny_globs, &env.deny_glob_cnt, env.cu_deny_globs[i]);
		if (err < 0) {
			return err;
		}
	}

	for (i = 0; i < env.cu_entry_glob_cnt; i++) {
		err = append_compile_unit(&env.ctx, &env.entry_globs, &env.entry_glob_cnt, env.cu_entry_globs[i]);
		if (err < 0) {
			return err;
		}
	}

	return err;
}

static int append_pid(int **pids, int *cnt, const char *arg)
{
	void *tmp;
	int pid;

	errno = 0;
	pid = strtol(arg, NULL, 10);
	if (errno || pid < 0) {
		fprintf(stderr, "Invalid PID: %d\n", pid);
		return -EINVAL;
	}

	tmp = realloc(*pids, (*cnt + 1) * sizeof(**pids));
	if (!tmp)
		return -ENOMEM;
	*pids = tmp;

	(*pids)[*cnt] = pid;
	*cnt = *cnt + 1;

	return 0;
}

static const char *err_map[] = {
	[0] = "NULL",
	[1] = "EPERM", [2] = "ENOENT", [3] = "ESRCH",
	[4] = "EINTR", [5] = "EIO", [6] = "ENXIO", [7] = "E2BIG",
	[8] = "ENOEXEC", [9] = "EBADF", [10] = "ECHILD", [11] = "EAGAIN",
	[12] = "ENOMEM", [13] = "EACCES", [14] = "EFAULT", [15] = "ENOTBLK",
	[16] = "EBUSY", [17] = "EEXIST", [18] = "EXDEV", [19] = "ENODEV",
	[20] = "ENOTDIR", [21] = "EISDIR", [22] = "EINVAL", [23] = "ENFILE",
	[24] = "EMFILE", [25] = "ENOTTY", [26] = "ETXTBSY", [27] = "EFBIG",
	[28] = "ENOSPC", [29] = "ESPIPE", [30] = "EROFS", [31] = "EMLINK",
	[32] = "EPIPE", [33] = "EDOM", [34] = "ERANGE", [35] = "EDEADLK",
	[36] = "ENAMETOOLONG", [37] = "ENOLCK", [38] = "ENOSYS", [39] = "ENOTEMPTY",
	[40] = "ELOOP", [42] = "ENOMSG", [43] = "EIDRM", [44] = "ECHRNG",
	[45] = "EL2NSYNC", [46] = "EL3HLT", [47] = "EL3RST", [48] = "ELNRNG",
	[49] = "EUNATCH", [50] = "ENOCSI", [51] = "EL2HLT", [52] = "EBADE",
	[53] = "EBADR", [54] = "EXFULL", [55] = "ENOANO", [56] = "EBADRQC",
	[57] = "EBADSLT", [59] = "EBFONT", [60] = "ENOSTR", [61] = "ENODATA",
	[62] = "ETIME", [63] = "ENOSR", [64] = "ENONET", [65] = "ENOPKG",
	[66] = "EREMOTE", [67] = "ENOLINK", [68] = "EADV", [69] = "ESRMNT",
	[70] = "ECOMM", [71] = "EPROTO", [72] = "EMULTIHOP", [73] = "EDOTDOT",
	[74] = "EBADMSG", [75] = "EOVERFLOW", [76] = "ENOTUNIQ", [77] = "EBADFD",
	[78] = "EREMCHG", [79] = "ELIBACC", [80] = "ELIBBAD", [81] = "ELIBSCN",
	[82] = "ELIBMAX", [83] = "ELIBEXEC", [84] = "EILSEQ", [85] = "ERESTART",
	[86] = "ESTRPIPE", [87] = "EUSERS", [88] = "ENOTSOCK", [89] = "EDESTADDRREQ",
	[90] = "EMSGSIZE", [91] = "EPROTOTYPE", [92] = "ENOPROTOOPT", [93] = "EPROTONOSUPPORT",
	[94] = "ESOCKTNOSUPPORT", [95] = "EOPNOTSUPP", [96] = "EPFNOSUPPORT", [97] = "EAFNOSUPPORT",
	[98] = "EADDRINUSE", [99] = "EADDRNOTAVAIL", [100] = "ENETDOWN", [101] = "ENETUNREACH",
	[102] = "ENETRESET", [103] = "ECONNABORTED", [104] = "ECONNRESET", [105] = "ENOBUFS",
	[106] = "EISCONN", [107] = "ENOTCONN", [108] = "ESHUTDOWN", [109] = "ETOOMANYREFS",
	[110] = "ETIMEDOUT", [111] = "ECONNREFUSED", [112] = "EHOSTDOWN", [113] = "EHOSTUNREACH",
	[114] = "EALREADY", [115] = "EINPROGRESS", [116] = "ESTALE", [117] = "EUCLEAN",
	[118] = "ENOTNAM", [119] = "ENAVAIL", [120] = "EISNAM", [121] = "EREMOTEIO",
	[122] = "EDQUOT", [123] = "ENOMEDIUM", [124] = "EMEDIUMTYPE", [125] = "ECANCELED",
	[126] = "ENOKEY", [127] = "EKEYEXPIRED", [128] = "EKEYREVOKED", [129] = "EKEYREJECTED",
	[130] = "EOWNERDEAD", [131] = "ENOTRECOVERABLE", [132] = "ERFKILL", [133] = "EHWPOISON",
	[512] = "ERESTARTSYS", [513] = "ERESTARTNOINTR", [514] = "ERESTARTNOHAND", [515] = "ENOIOCTLCMD",
	[516] = "ERESTART_RESTARTBLOCK", [517] = "EPROBE_DEFER", [518] = "EOPENSTALE", [519] = "ENOPARAM",
	[521] = "EBADHANDLE", [522] = "ENOTSYNC", [523] = "EBADCOOKIE", [524] = "ENOTSUPP",
	[525] = "ETOOSMALL", [526] = "ESERVERFAULT", [527] = "EBADTYPE", [528] = "EJUKEBOX",
	[529] = "EIOCBQUEUED", [530] = "ERECALLCONFLICT",
};

static int str_to_err(const char *arg)
{
	int i;

	/* doesn't matter if it's -Exxx or Exxx */
	if (arg[0] == '-')
		arg++;

	for (i = 0; i < ARRAY_SIZE(err_map); i++) {
		if (!err_map[i])
			continue;

		if (strcmp(arg, err_map[i]) != 0)
			continue;

		return i;
	}

	fprintf(stderr, "Unrecognized error '%s'\n", arg);
	return -ENOENT;
}

static const char *err_to_str(long err) {

	if (err < 0)
		err = -err;
	if (err < ARRAY_SIZE(err_map))
		return err_map[err];
	return NULL;
}

static void err_mask_set(__u64 *err_mask, int err_value)
{
	err_mask[err_value / 64] |= 1ULL << (err_value % 64);
}

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	int i, j, err;

	switch (key) {
	case 'V':
		env.show_version = true;
		break;
	case 'v':
		env.verbose = true;
		if (arg) {
			if (strcmp(arg, "v") == 0) {
				env.debug = true;
			} else if (strcmp(arg, "vv") == 0) {
				env.debug = true;
				env.debug_extra = true;
			} else {
				fprintf(stderr,
					"Unrecognized verbosity setting '%s', only -v, -vv, and -vvv are supported\n",
					arg);
				return -EINVAL;
			}
		}
		break;
	case 'l':
		env.bpf_logs = true;
		break;
	case 'c':
		for (i = 0; i < ARRAY_SIZE(presets); i++) {
			const struct preset *p = &presets[i];
			const char *glob;

			if (strcmp(p->name, arg) != 0)
				continue;

			for (j = 0; p->entry_globs[j]; j++) {
				glob = p->entry_globs[j];
				if (append_str(&env.entry_globs, &env.entry_glob_cnt, glob))
					return -ENOMEM;
			}
			for (j = 0; p->allow_globs[j]; j++) {
				glob = p->allow_globs[j];
				if (append_str(&env.allow_globs, &env.allow_glob_cnt, glob))
					return -ENOMEM;
			}
			for (j = 0; p->deny_globs[j]; j++) {
				glob = p->deny_globs[j];
				if (append_str(&env.deny_globs, &env.deny_glob_cnt, glob))
					return -ENOMEM;
			}

			return 0;
		}
		fprintf(stderr, "Unknown preset '%s' specified.\n", arg);
		break;
	case 'a':
		if (arg[0] == '@') {
			err = append_str_file(&env.allow_globs, &env.allow_glob_cnt, arg + 1);
		} else if (arg[0] == ':') {
			err = append_str(&env.cu_allow_globs, &env.cu_allow_glob_cnt, arg + 1);
		} else {
			err = append_str(&env.allow_globs, &env.allow_glob_cnt, arg);
		}
		if (err)
			return err;
		break;
	case 'd':
		if (arg[0] == '@') {
			err = append_str_file(&env.deny_globs, &env.deny_glob_cnt, arg + 1);
		} else if (arg[0] == ':') {
			err = append_str(&env.cu_deny_globs, &env.cu_deny_glob_cnt, arg + 1);
		} else {
			err = append_str(&env.deny_globs, &env.deny_glob_cnt, arg);
		}
		if (err)
			return err;
		break;
	case 'e':
		if (arg[0] == '@') {
			err = append_str_file(&env.entry_globs, &env.entry_glob_cnt, arg + 1);
		} else if (arg[0] == ':') {
			err = append_str(&env.cu_entry_globs, &env.cu_entry_glob_cnt, arg + 1);
		} else {
			err = append_str(&env.entry_globs, &env.entry_glob_cnt, arg);
		}
		if (err)
			return err;
		break;
	case 's':
		env.symb_mode = SYMB_LINEINFO;
		if (arg) {
			if (strcmp(arg, "none") == 0 || strcmp(arg, "n") == 0) {
				env.symb_mode = SYMB_NONE;
			} else if (strcmp(arg, "inlines") == 0 || strcmp(arg, "s") == 0) {
				env.symb_mode |= SYMB_INLINES;
			} else {
				fprintf(stderr,
					"Unrecognized symbolization setting '%s', only -s, -ss (-s inlines), and -sn (-s none) are supported\n",
					arg);
				return -EINVAL;
			}
		}
		break;
	case 'k':
		env.vmlinux_path = arg;
		break;
	case 'n':
		if (arg[0] == '@') {
			err = append_str_file(&env.allow_comms, &env.allow_comm_cnt, arg + 1);
			if (err)
				return err;
		} else if (append_str(&env.allow_comms, &env.allow_comm_cnt, arg)) {
			return -ENOMEM;
		}
		break;
	case 'N':
		if (arg[0] == '@') {
			err = append_str_file(&env.deny_comms, &env.deny_comm_cnt, arg + 1);
			if (err)
				return err;
		} else if (append_str(&env.deny_comms, &env.deny_comm_cnt, arg)) {
			return -ENOMEM;
		}
		break;
	case 'p':
		err = append_pid(&env.allow_pids, &env.allow_pid_cnt, arg);
		if (err)
			return err;
		break;
	case 'P':
		err = append_pid(&env.deny_pids, &env.deny_pid_cnt, arg);
		if (err)
			return err;
		break;
	case 'x':
		err = str_to_err(arg);
		if (err < 0)
			return err;
		/* we start out with all errors allowed, but as soon as we get
		 * the first allowed error specified, we need to reset
		 * all the error to be not allowed by default
		 */
		if (env.allow_error_cnt == 0)
			memset(env.allow_error_mask, 0, sizeof(env.allow_error_mask));
		env.allow_error_cnt++;
		env.has_error_filter = true;
		err_mask_set(env.allow_error_mask, err);
		break;
	case 'X':
		err = str_to_err(arg);
		if (err < 0)
			return err;
		/* we don't need to do anything extra for error blacklist,
		 * because we start with no errors blacklisted by default
		 * anyways, which differs from the logic for error whitelist
		 */
		env.has_error_filter = true;
		err_mask_set(env.deny_error_mask, err);
		break;
	case 'S':
		env.emit_success_stacks = true;
		break;
	case 'M':
		if (env.attach_mode != ATTACH_DEFAULT) {
			fprintf(stderr, "Can't specify -M, -K or -F simultaneously, pick one.\n");
			return -EINVAL;
		}
		env.attach_mode = ATTACH_KPROBE_MULTI;
		break;
	case 'K':
		if (env.attach_mode != ATTACH_DEFAULT) {
			fprintf(stderr, "Can't specify -M, -K or -F simultaneously, pick one.\n");
			return -EINVAL;
		}
		env.attach_mode = ATTACH_KPROBE_SINGLE;
		break;
	case 'F':
		if (env.attach_mode != ATTACH_DEFAULT) {
			fprintf(stderr, "Can't specify -M, -K or -F simultaneously, pick one.\n");
			return -EINVAL;
		}
		env.attach_mode = ATTACH_FENTRY;
		break;
	case 'A':
		env.emit_intermediate_stacks = true;
		break;
	case 'L':
		errno = 0;
		env.longer_than_ms = strtol(arg, NULL, 10);
		if (errno || env.longer_than_ms <= 0) {
			fprintf(stderr, "Invalid -L duration: %d\n", env.longer_than_ms);
			return -EINVAL;
		}
		break;
	case OPT_LBR:
		env.use_lbr = true;
		if (arg && sscanf(arg, "%li", &env.lbr_flags) != 1) {
			err = -errno;
			fprintf(stderr, "Failed to parse LBR flags spec '%s': %d\n",
				arg, err);
			return -EINVAL;
		}
		break;
	case OPT_FULL_STACKS:
		env.emit_full_stacks = true;
		break;
	case OPT_STACKS_MAP_SIZE:
		errno = 0;
		env.stacks_map_sz = strtol(arg, NULL, 10);
		if (errno || env.pid < 0) {
			fprintf(stderr, "Invalid stacks map size: %d\n", env.stacks_map_sz);
			return -EINVAL;
		}
		break;
	case OPT_DRY_RUN:
		env.dry_run = true;
		break;
	case ARGP_KEY_ARG:
		argp_usage(state);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static const struct argp argp = {
	.options = opts,
	.parser = parse_arg,
	.doc = argp_program_doc,
};

static __u64 ktime_off;

static __u64 timespec_to_ns(struct timespec *ts)
{
	return ts->tv_sec * 1000000000ULL + ts->tv_nsec;
}

static void calibrate_ktime(void)
{
	int i;
	struct timespec t1, t2, t3;
	__u64 best_delta = 0, delta, ts;

	for (i = 0; i < 10; i++) {
		clock_gettime(CLOCK_REALTIME, &t1);
		clock_gettime(CLOCK_MONOTONIC, &t2);
		clock_gettime(CLOCK_REALTIME, &t3);

		delta = timespec_to_ns(&t3) - timespec_to_ns(&t1);
		ts = (timespec_to_ns(&t3) + timespec_to_ns(&t1)) / 2;

		if (i == 0 || delta < best_delta) {
			best_delta = delta;
			ktime_off = ts - timespec_to_ns(&t2);
		}
	}
}

static void ts_to_str(__u64 ts, char buf[], size_t buf_sz)
{
	char tmp[32];
	time_t t = ts / 1000000000;
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(tmp, sizeof(tmp), "%H:%M:%S", &tm);

	snprintf(buf, buf_sz, "%s.%03lld", tmp, ts / 1000000 % 1000);
}

/* PRESETS */

static const char *bpf_entry_globs[] = {
	"*_sys_bpf",
	NULL,
};

static const char *bpf_allow_globs[] = {
	"*bpf*",
	"*btf*",
	"do_check*",
	"reg_*",
	"check_*",
	"resolve_*",
	"convert_*",
	"adjust_*",
	"sanitize_*",
	"map_*",
	"ringbuf_*",
	"array_*",
	"__vmalloc_*",
	"__alloc*",
	"pcpu_*",
	"memdup_*",
	"stack_map_*",
	"htab_*",
	"generic_map_*",
	"*copy_from*",
	"*copy_to*",
	NULL,
};

static const char *bpf_deny_globs[] = {
	"bpf_get_smp_processor_id",
	"bpf_get_current_pid_tgid",
	"*migrate*",
	"rcu_read_lock*",
	"rcu_read_unlock*",

	/* too noisy */
	"bpf_lsm_*",
	"check_cfs_rq_runtime",
	"find_busiest_group",
	"find_vma*",

	/* non-failing */
	"btf_sec_info_cmp",

	/* can't attach for some reason */
	"copy_to_user_nofault",

	NULL,
};

static const char *perf_entry_globs[] = {
	"*_sys__perf_event_open",
	"perf_ioctl",
	NULL,
};

static const char *perf_allow_globs[] = {
	"*perf_*",
	NULL,
};

static const char *perf_deny_globs[] = {
	NULL,
};

/* fexit logical stack trace item */
struct fstack_item {
	const struct mass_attacher_func_info *finfo;
	const char *name;
	long res;
	long lat;
	bool finished;
	bool stitched;
	bool err_start;
};

static bool is_err_in_mask(__u64 *err_mask, int err)
{
	if (err < 0)
		err = -err;
	if (err >= MAX_ERR_CNT)
		return false;
	return (err_mask[err / 64] >> (err % 64)) & 1;
}

static bool should_report_stack(struct ctx *ctx, const struct call_stack *s)
{
	struct retsnoop_bpf *skel = ctx->skel;
	int i, id, flags, res;
	bool allowed = false;

	if (!env.has_error_filter)
		return true;

	for (i = 0; i < s->max_depth; i++) {
		id = s->func_ids[i];
		flags = skel->bss->func_flags[id];

		if (flags & FUNC_CANT_FAIL)
			continue;

		res = s->func_res[i];
		if (flags & FUNC_NEEDS_SIGN_EXT)
			res = (long)(int)res;

		if (res == 0 && !(flags & FUNC_RET_PTR))
			continue;

		/* if error is blacklisted, reject immediately */
		if (is_err_in_mask(env.deny_error_mask, res))
			return false;
		/* if error is whitelisted, mark as allowed; but we need to
		 * still see if any other errors in the stack are blacklisted
		 */
		if (is_err_in_mask(env.allow_error_mask, res))
			allowed = true;
	}

	/* no stitched together stack */
	if (s->max_depth + 1 != s->saved_depth)
		return allowed;

	for (i = s->saved_depth - 1; i < s->saved_max_depth; i++) {
		id = s->saved_ids[i];
		flags = skel->bss->func_flags[id];

		if (flags & FUNC_CANT_FAIL)
			continue;

		res = s->func_res[i];
		if (flags & FUNC_NEEDS_SIGN_EXT)
			res = (long)(int)res;

		if (res == 0 && !(flags & FUNC_RET_PTR))
			continue;

		/* if error is blacklisted, reject immediately */
		if (is_err_in_mask(env.deny_error_mask, res))
			return false;
		/* if error is whitelisted, mark as allowed; but we need to
		 * still see if any other errors in the stack are blacklisted
		 */
		if (is_err_in_mask(env.allow_error_mask, res))
			allowed = true;
	}

	return allowed;
}

static int filter_fstack(struct ctx *ctx, struct fstack_item *r, const struct call_stack *s)
{
	const struct mass_attacher_func_info *finfo;
	struct mass_attacher *att = ctx->att;
	struct retsnoop_bpf *skel = ctx->skel;
	struct fstack_item *fitem;
	const char *fname;
	int i, id, flags, cnt;

	for (i = 0, cnt = 0; i < s->max_depth; i++, cnt++) {
		id = s->func_ids[i];
		flags = skel->bss->func_flags[id];
		finfo = mass_attacher__func(att, id);
		fname = finfo->name;

		fitem = &r[cnt];
		fitem->finfo = finfo;
		fitem->name = fname;
		fitem->stitched = false;
		if (i >= s->depth) {
			fitem->finished = true;
			fitem->lat = s->func_lat[i];
		} else {
			fitem->finished = false;
			fitem->lat = 0;
		}
		if (flags & FUNC_NEEDS_SIGN_EXT)
			fitem->res = (long)(int)s->func_res[i];
		else
			fitem->res = s->func_res[i];
		fitem->lat = s->func_lat[i];
	}

	/* no stitched together stack */
	if (s->max_depth + 1 != s->saved_depth)
		return cnt;

	for (i = s->saved_depth - 1; i < s->saved_max_depth; i++, cnt++) {
		id = s->saved_ids[i];
		flags = skel->bss->func_flags[id];
		finfo = mass_attacher__func(att, id);
		fname = finfo->name;

		fitem = &r[cnt];
		fitem->finfo = finfo;
		fitem->name = fname;
		fitem->stitched = true;
		fitem->finished = true;
		fitem->lat = s->saved_lat[i];
		if (flags & FUNC_NEEDS_SIGN_EXT)
			fitem->res = (long)(int)s->saved_res[i];
		else
			fitem->res = s->saved_res[i];
	}

	return cnt;
}

/* actual kernel stack trace item */
struct kstack_item {
	const struct ksym *ksym;
	long addr;
	bool filtered;
};

static bool is_bpf_tramp(const struct kstack_item *item)
{
	static char bpf_tramp_pfx[] = "bpf_trampoline_";

	if (!item->ksym)
		return false;

	return strncmp(item->ksym->name, bpf_tramp_pfx, sizeof(bpf_tramp_pfx) - 1) == 0
	       && isdigit(item->ksym->name[sizeof(bpf_tramp_pfx)]);
}

static bool is_bpf_prog(const struct kstack_item *item)
{
	static char bpf_prog_pfx[] = "bpf_prog_";

	if (!item->ksym)
		return false;

	return strncmp(item->ksym->name, bpf_prog_pfx, sizeof(bpf_prog_pfx) - 1) == 0
	       && isxdigit(item->ksym->name[sizeof(bpf_prog_pfx)]);
}

#define FTRACE_OFFSET 0x5

static int filter_kstack(struct ctx *ctx, struct kstack_item *r, const struct call_stack *s)
{
	struct ksyms *ksyms = ctx->ksyms;
	int i, n, p;

	/* lookup ksyms and reverse stack trace to match natural call order */
	n = s->kstack_sz / 8;
	for (i = 0; i < n; i++) {
		struct kstack_item *item = &r[n - i - 1];

		item->addr = s->kstack[i];
		item->filtered = false;
		item->ksym = ksyms__map_addr(ksyms, item->addr);
		if (!item->ksym)
			continue;
	}

	/* perform addiitonal post-processing to filter out bpf_trampoline and
	 * bpf_prog symbols, fixup fexit patterns, etc
	 */
	for (i = 0, p = 0; i < n; i++) {
		struct kstack_item *item = &r[p];

		*item = r[i];

		if (!item->ksym) {
			p++;
			continue;
		}

		/* Ignore bpf_trampoline frames and fix up stack traces.
		 * When fexit program happens to be inside the stack trace,
		 * a following stack trace pattern will be apparent (taking into account inverted order of frames
		 * which we did few lines above):
		 *     ffffffff8116a3d5 bpf_map_alloc_percpu+0x5
		 *     ffffffffa16db06d bpf_trampoline_6442494949_0+0x6d
		 *     ffffffff8116a40f bpf_map_alloc_percpu+0x3f
		 * 
		 * bpf_map_alloc_percpu+0x5 is real, by it just calls into the
		 * trampoline, which them calls into original call
		 * (bpf_map_alloc_percpu+0x3f). So the last item is what
		 * really matters, everything else is just a distraction, so
		 * try to detect this and filter it out. Unless we are in
		 * full-stacks mode, of course, in which case we live a hint
		 * that this would be filtered out (helps with debugging
		 * overall), but otherwise is preserved.
		 */
		if (i + 2 < n && is_bpf_tramp(&r[i + 1])
		    && r[i].ksym == r[i + 2].ksym
		    && r[i].addr - r[i].ksym->addr == FTRACE_OFFSET) {
			if (env.emit_full_stacks) {
				item->filtered = true;
				p++;
				continue;
			}

			/* skip two elements and process useful item */
			*item = r[i + 2];
			continue;
		}

		/* Ignore bpf_trampoline and bpf_prog in stack trace, those
		 * are most probably part of our own instrumentation, but if
		 * not, you can still see them in full-stacks mode.
		 * Similarly, remove bpf_get_stack_raw_tp, which seems to be
		 * always there due to call to bpf_get_stack() from BPF
		 * program.
		 */
		if (is_bpf_tramp(&r[i]) || is_bpf_prog(&r[i])
		    || strcmp(r[i].ksym->name, "bpf_get_stack_raw_tp") == 0) {
			if (env.emit_full_stacks) {
				item->filtered = true;
				p++;
				continue;
			}

			if (i + 1 < n)
				*item = r[i + 1];
			continue;
		}

		p++;
	}

	return p;
}

static int detect_linux_src_loc(const char *path)
{
	static const char *linux_dirs[] = {
		"arch/", "kernel/", "include/", "block/", "fs/", "net/",
		"drivers/", "mm/", "ipc/", "security/", "lib/", "crypto/",
		"certs/", "init/", "lib/", "scripts/", "sound/", "tools/",
		"usr/", "virt/", 
	};
	int i;
	char *p;

	for (i = 0; i < ARRAY_SIZE(linux_dirs); i++) {
		p = strstr(path, linux_dirs[i]);
		if (p)
			return p - path;
	}

	return 0;
}

static void print_item(struct ctx *ctx, const struct fstack_item *fitem, const struct kstack_item *kitem)
{
	const int err_width = 12;
	const int lat_width = 12;
	static struct a2l_resp resps[64];
	struct a2l_resp *resp = NULL;
	int symb_cnt = 0, i, line_off, p = 0;
	const char *fname;
	int src_print_off = 70, func_print_off;

	if (env.symb_mode != SYMB_NONE && ctx->a2l && kitem && !kitem->filtered) {
		long addr = kitem->addr;

		if (kitem->ksym && kitem->ksym && kitem->ksym->addr - kitem->addr == FTRACE_OFFSET)
			addr -= FTRACE_OFFSET;

		symb_cnt = addr2line__symbolize(ctx->a2l, addr, resps);
		if (symb_cnt < 0)
			symb_cnt = 0;
		if (symb_cnt > 0)
			resp = &resps[symb_cnt - 1];
	}

	/* this should be rare, either a bug or we couldn't get valid kernel
	 * stack trace
	 */
	if (!kitem)
		p += printf("!");
	else
		p += printf(" ");

	p += printf("%c ", (fitem && fitem->stitched) ? '*' : ' ');

	if (fitem && !fitem->finished) {
		p += printf("%*s %-*s ", lat_width, "...", err_width, "[...]");
	} else if (fitem) {
		p += printf("%*ldus ", lat_width - 2 /* for "us" */, fitem->lat / 1000);
		if (fitem->res == 0) {
			p += printf("%-*s ", err_width, "[NULL]");
		} else {
			const char *errstr;
			int print_cnt;

			errstr = err_to_str(fitem->res);
			if (errstr)
				print_cnt = printf("[-%s]", errstr);
			else
				print_cnt = printf("[%ld]", fitem->res);
			p += print_cnt;
			p += printf("%*s ", err_width - print_cnt, "");
		}
	} else {
		p += printf("%*s ", lat_width + 1 + err_width, "");
	}

	if (env.emit_full_stacks) {
		if (kitem && kitem->filtered) 
			p += printf("~%016lx ", kitem->addr);
		else if (kitem)
			p += printf(" %016lx ", kitem->addr);
		else
			p += printf(" %*s ", 16, "");
	}

	if (kitem && kitem->ksym)
		fname = kitem->ksym->name;
	else if (fitem)
		fname = fitem->name;
	else
		fname = "";

	func_print_off = p;
	p += printf("%s", fname);
	if (kitem && kitem->ksym)
		p += printf("+0x%lx", kitem->addr - kitem->ksym->addr);
	if (symb_cnt) {
		if (env.emit_full_stacks)
			src_print_off += 18; /* for extra " %16lx " */
		p += printf(" %*s(", p < src_print_off ? src_print_off - p : 0, "");

		if (strcmp(fname, resp->fname) != 0)
			p += printf("%s @ ", resp->fname);

		line_off = detect_linux_src_loc(resp->line);
		p += printf("%s)", resp->line + line_off);
	}

	p += printf("\n");

	for (i = 1, resp--; i < symb_cnt; i++, resp--) {
		p = printf("%*s. %s", func_print_off, "", resp->fname);
		line_off = detect_linux_src_loc(resp->line);
		printf(" %*s(%s)\n",
		       p < src_print_off ? src_print_off - p : 0, "",
		       resp->line + line_off);
	}
}

static void emit_lbr(struct ctx *ctx, const char *pfx, long addr)
{
	static struct a2l_resp resps[64];
	struct a2l_resp *resp = NULL;
	int symb_cnt = 0, line_off, i;
	const struct ksym *ksym;

	ksym = ksyms__map_addr(ctx->ksyms, addr);
	if (ksym) {
		printf("%s%s+0x%lx", pfx, ksym->name, addr - ksym->addr);
	} else {
		printf("%s", pfx);
	}

	if (!ctx->a2l || env.symb_mode == SYMB_NONE) {
		printf("\n");
		return;
	}

	symb_cnt = addr2line__symbolize(ctx->a2l, addr, resps);
	if (symb_cnt <= 0) {
		printf("\n");
		return;
	}

	resp = &resps[symb_cnt - 1];

	line_off = detect_linux_src_loc(resp->line);
	printf(" (%s)\n", resp->line + line_off);

	for (i = 1, resp--; i < symb_cnt; i++, resp--) {
		printf("\t\t. %s", resp->fname);
		line_off = detect_linux_src_loc(resp->line);
		printf(" (%s)\n", resp->line + line_off);
	}
}

static bool lbr_matches(unsigned long addr, unsigned long start, unsigned long end)
{
	if (!start)
		return true;

	return start <= addr && addr < end;
}

static int handle_event(void *ctx, void *data, size_t data_sz)
{
	static struct fstack_item fstack[MAX_FSTACK_DEPTH];
	static struct kstack_item kstack[MAX_KSTACK_DEPTH];
	const struct fstack_item *fitem;
	const struct kstack_item *kitem;
	struct ctx *dctx = ctx;
	const struct call_stack *s = data;
	int i, j, fstack_n, kstack_n;
	char timestamp[64];

	if (!s->is_err && !env.emit_success_stacks)
		return 0;

	if (s->is_err && env.has_error_filter && !should_report_stack(dctx, s))
		return 0;

	if (env.debug) {
		printf("GOT %s STACK (depth %u):\n", s->is_err ? "ERROR" : "SUCCESS", s->max_depth);
		printf("DEPTH %d MAX DEPTH %d SAVED DEPTH %d MAX SAVED DEPTH %d\n",
				s->depth, s->max_depth, s->saved_depth, s->saved_max_depth);
	}

	fstack_n = filter_fstack(dctx, fstack, s);
	if (fstack_n < 0) {
		fprintf(stderr, "FAILURE DURING FILTERING FUNCTION STACK!!! %d\n", fstack_n);
		return -1;
	}
	kstack_n = filter_kstack(dctx, kstack, s);
	if (kstack_n < 0) {
		fprintf(stderr, "FAILURE DURING FILTERING KERNEL STACK!!! %d\n", kstack_n);
		return -1;
	}
	if (env.debug) {
		printf("FSTACK (%d items):\n", fstack_n);
		printf("KSTACK (%d items out of original %ld):\n", kstack_n, s->kstack_sz / 8);
	}

	ts_to_str(s->emit_ts + ktime_off, timestamp, sizeof(timestamp));
	printf("%s PID %d (%s):\n", timestamp, s->pid, s->comm);

	i = 0;
	j = 0;
	while (i < fstack_n) {
		fitem = &fstack[i];
		kitem = j < kstack_n ? &kstack[j] : NULL;

		if (!kitem) {
			/* this shouldn't happen unless we got no kernel stack
			 * or there is some bug
			 */
			print_item(dctx, fitem, NULL);
			i++;
			continue;
		}

		/* exhaust unknown kernel stack items, assuming we should find
		 * kstack_item matching current fstack_item eventually, which
		 * should be the case when kernel stack trace is correct
		 */
		if (!kitem->ksym || kitem->filtered
		    || strcmp(kitem->ksym->name, fitem->name) != 0) {
			print_item(dctx, NULL, kitem);
			j++;
			continue;
		}

		/* happy case, lots of info, yay */
		print_item(dctx, fitem, kitem);
		i++;
		j++;
		continue;
	}

	for (; j < kstack_n; j++) {
		print_item(dctx, NULL, &kstack[j]);
	}

	if (env.use_lbr) {
		unsigned long start = 0, end = 0;
		int lbr_cnt, lbr_to = 0;

		if (s->lbrs_sz < 0) {
			fprintf(stderr, "Failed to capture LBR entries: %ld\n", s->lbrs_sz);
			goto out;
		}

		if (fstack_n > 0) {
			fitem = &fstack[fstack_n - 1];
			if (fitem->finfo->size) {
				start = fitem->finfo->addr;
				end = fitem->finfo->addr + fitem->finfo->size;
			}
		}

		lbr_cnt = s->lbrs_sz / sizeof(struct perf_branch_entry);

		if (!env.emit_full_stacks) {
			/* Filter out last few irrelevant LBRs that captured
			 * internal BPF/kprobe/perf jumps. For that, find the
			 * first LBR record that overlaps with the last traced
			 * function. All the records after that are assumed
			 * relevant.
			 */
			for (i = 0, lbr_to = 0; i < lbr_cnt; i++, lbr_to++) {
				if (lbr_matches(s->lbrs[i].from, start, end) ||
				    lbr_matches(s->lbrs[i].to, start, end))
					break;
			}
		}

		for (i = lbr_cnt - 1; i >= (lbr_to == lbr_cnt ? 0 : lbr_to); i--) {

			printf("[LBR #%02d] 0x%016lx -> 0x%016lx\n",
			       i, (long)s->lbrs[i].from, (long)s->lbrs[i].to);

			emit_lbr(dctx, "<-\t", s->lbrs[i].from);
			emit_lbr(dctx, "->\t", s->lbrs[i].to);
		}

		if (lbr_to == lbr_cnt)
			printf("[LBR] No relevant LBR data were captured, showing unfiltered LBR stack!\n");
	}

out:
	printf("\n\n");

	return 0;
}

static void handle_event_pb(void *ctx, int cpu, void *data, unsigned data_sz)
{
	(void)handle_event(ctx, data, data_sz);
}

static int func_flags(const char *func_name, const struct btf *btf, int btf_id)
{
	const struct btf_type *t;

	if (!btf_id) {
		/* for kprobes-only functions we might not have BTF info,
		 * so assume int-returning failing function as the most common
		 * case
		 */
		return FUNC_NEEDS_SIGN_EXT;
	}

	/* FUNC */
	t = btf__type_by_id(btf, btf_id);

	/* FUNC_PROTO */
	t = btf__type_by_id(btf, t->type);

	/* check FUNC_PROTO's return type for VOID */
	if (!t->type)
		return FUNC_CANT_FAIL | FUNC_RET_VOID;

	t = btf__type_by_id(btf, t->type);
	while (btf_is_mod(t) || btf_is_typedef(t))
		t = btf__type_by_id(btf, t->type);

	if (btf_is_ptr(t))
		return FUNC_RET_PTR; /* can fail, no sign extension */

	/* unsigned is treated as non-failing */
	if (btf_is_int(t)) {
		if (btf_int_encoding(t) & BTF_INT_BOOL)
			return FUNC_CANT_FAIL | FUNC_RET_BOOL;
		if (!(btf_int_encoding(t) & BTF_INT_SIGNED))
			return FUNC_CANT_FAIL;
	}

	/* byte and word are treated as non-failing */
	if (t->size < 4)
		return FUNC_CANT_FAIL;

	/* integers need sign extension */
	if (t->size == 4)
		return FUNC_NEEDS_SIGN_EXT;

	return 0;
}

static bool func_filter(const struct mass_attacher *att,
			const struct btf *btf, int func_btf_id,
			const char *name, int func_id)
{
	/* no extra filtering for now */
	return true;
}

static int find_vmlinux(char *path, size_t max_len, bool soft)
{
	const char *locations[] = {
		"/boot/vmlinux-%1$s",
		"/lib/modules/%1$s/vmlinux-%1$s",
		"/lib/modules/%1$s/build/vmlinux",
		"/usr/lib/modules/%1$s/kernel/vmlinux",
		"/usr/lib/debug/boot/vmlinux-%1$s",
		"/usr/lib/debug/boot/vmlinux-%1$s.debug",
		"/usr/lib/debug/lib/modules/%1$s/vmlinux",
	};
	struct utsname buf;
	int i;

	uname(&buf);

	for (i = 0; i < ARRAY_SIZE(locations); i++) {
		snprintf(path, PATH_MAX, locations[i], buf.release);

		if (access(path, R_OK)) {
			if (env.debug)
				printf("No vmlinux image at %s found...\n", path);
			continue;
		}

		if (env.verbose)
			printf("Using vmlinux image at %s.\n", path);

		return 0;
	}

	if (!soft || env.verbose)
		fprintf(soft ? stdout : stderr, "Failed to locate vmlinux image location. Please use -k <vmlinux-path> to specify explicitly.\n");

	path[0] = '\0';

	return -ESRCH;
}

static int detect_kernel_features(void)
{
	struct calib_feat_bpf *skel;
	int err;

	skel = calib_feat_bpf__open_and_load();
	if (!skel) {
		fprintf(stderr, "Failed to load feature detection skeleton\n");
		return -EFAULT;
	}

	skel->bss->my_tid = syscall(SYS_gettid);

	err = calib_feat_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach feature detection skeleton\n");
		calib_feat_bpf__destroy(skel);
		return -EFAULT;
	}

	usleep(1);

	if (env.debug) {
		printf("Feature detection results:\n"
		       "\tBPF ringbuf map supported: %s\n"
		       "\tbpf_get_func_ip() supported: %s\n"
		       "\tbpf_get_branch_snapshot() supported: %s\n"
		       "\tBPF cookie supported: %s\n"
		       "\tmulti-attach kprobe supported: %s\n",
		       skel->bss->has_ringbuf ? "yes" : "no",
		       skel->bss->has_bpf_get_func_ip ? "yes" : "no",
		       skel->bss->has_branch_snapshot ? "yes" : "no",
		       skel->bss->has_bpf_cookie ? "yes" : "no",
		       skel->bss->has_kprobe_multi ? "yes" : "no");
	}

	env.has_ringbuf = skel->bss->has_ringbuf;
	env.has_branch_snapshot = skel->bss->has_branch_snapshot;

	calib_feat_bpf__destroy(skel);
	return 0;
}

#define INTEL_FIXED_VLBR_EVENT        0x1b00

static int create_lbr_perf_events(int *fds, int cpu_cnt)
{
	struct perf_event_attr attr;
	int cpu, err;

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.type = PERF_TYPE_HARDWARE;
	attr.config = PERF_COUNT_HW_CPU_CYCLES;
	attr.sample_type = PERF_SAMPLE_BRANCH_STACK;
	attr.branch_sample_type = PERF_SAMPLE_BRANCH_KERNEL |
				  (env.lbr_flags ?: PERF_SAMPLE_BRANCH_ANY);

	if (env.debug)
		printf("LBR flags are 0x%lx\n", (long)attr.branch_sample_type);

	for (cpu = 0; cpu < env.cpu_cnt; cpu++) {
		fds[cpu] = syscall(__NR_perf_event_open, &attr, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC);
		if (fds[cpu] < 0) {
			err = -errno;
			for (cpu--; cpu >= 0; cpu--) {
				close(fds[cpu]);
				fds[cpu] = -1;
			}
			return err;
		}
	}

	return 0;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !env.debug_extra)
		return 0;
	return vfprintf(stderr, format, args);
}

static volatile sig_atomic_t exiting;

static void sig_handler(int sig)
{
	exiting = true;
}

int main(int argc, char **argv)
{
	long page_size = sysconf(_SC_PAGESIZE);
	struct mass_attacher_opts att_opts = {};
	const struct btf *vmlinux_btf = NULL;
	struct mass_attacher *att = NULL;
	struct retsnoop_bpf *skel = NULL;
	struct ring_buffer *rb = NULL;
	struct perf_buffer *pb = NULL;
	int *lbr_perf_fds = NULL;
	char vmlinux_path[1024] = {};
	int err, i, j, n;
	__u64 ts1, ts2;

	if (setvbuf(stdout, NULL, _IOLBF, BUFSIZ))
		fprintf(stderr, "Failed to set output mode to line-buffered!\n");

	/* set allowed error mask to all 1s (enabled by default) */
	memset(env.allow_error_mask, 0xFF, sizeof(env.allow_error_mask));

	/* Parse command line arguments */
	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return -1;

	if (env.show_version) {
		printf("%s\n", argp_program_version);
		return 0;
	}

	if (env.entry_glob_cnt == 0) {
		fprintf(stderr, "No entry point globs specified. "
				"Please provide entry glob(s) ('-e GLOB') and/or any preset ('-p PRESET').\n");
		return -1;
	}

	if (geteuid() != 0)
		fprintf(stderr, "You are not running as root! Expect failures. Please use sudo or run as root.\n");

	if (env.symb_mode == SYMB_DEFAULT && !env.vmlinux_path) {
		if (find_vmlinux(vmlinux_path, sizeof(vmlinux_path), true /* soft */))
			env.symb_mode = SYMB_NONE;
	}

	if (env.symb_mode != SYMB_NONE || env.cu_allow_glob_cnt || env.cu_deny_glob_cnt || env.cu_entry_glob_cnt) {
		bool symb_inlines = false;;

		if (!env.vmlinux_path &&
		    vmlinux_path[0] == '\0' &&
		    find_vmlinux(vmlinux_path, sizeof(vmlinux_path), false /* hard error */))
			return -1;

		if (env.symb_mode == SYMB_DEFAULT || (env.symb_mode & SYMB_INLINES))
			symb_inlines = true;

		env.ctx.a2l = addr2line__init(env.vmlinux_path ?: vmlinux_path, symb_inlines);
		if (!env.ctx.a2l) {
			fprintf(stderr, "Failed to start addr2line for vmlinux image at %s!\n",
				env.vmlinux_path ?: vmlinux_path);
			return -1;
		}
	}

	if (process_cu_globs()) {
		fprintf(stderr, "Failed to process file paths.\n");
		return -1;
	}

	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	/* determine mapping from bpf_ktime_get_ns() to real clock */
	calibrate_ktime();

	/* Set up libbpf errors and debug info callback */
	libbpf_set_print(libbpf_print_fn);

	if (detect_kernel_features()) {
		fprintf(stderr, "Kernel feature detection failed.\n");
		return -1;
	}

	env.cpu_cnt = libbpf_num_possible_cpus();
	if (env.cpu_cnt <= 0) {
		fprintf(stderr, "Failed to determine number of CPUs: %d\n", env.cpu_cnt);
		return -1;
	}

	/* Open BPF skeleton */
	env.ctx.skel = skel = retsnoop_bpf__open();
	if (!skel) {
		fprintf(stderr, "Failed to open BPF skeleton.\n");
		return -1;
	}

	bpf_map__set_max_entries(skel->maps.stacks, env.stacks_map_sz);

	skel->rodata->tgid_allow_cnt = env.allow_pid_cnt;
	skel->rodata->tgid_deny_cnt = env.deny_pid_cnt;
	if (env.allow_pid_cnt + env.deny_pid_cnt > 0) {
		bpf_map__set_max_entries(skel->maps.tgids_filter,
					 env.allow_pid_cnt + env.deny_pid_cnt);
	}

	skel->rodata->comm_allow_cnt = env.allow_comm_cnt;
	skel->rodata->comm_deny_cnt = env.deny_comm_cnt;
	if (env.allow_comm_cnt + env.deny_comm_cnt > 0) {
		bpf_map__set_max_entries(skel->maps.comms_filter,
					 env.allow_comm_cnt + env.deny_comm_cnt);
	}

	/* turn on extra bpf_printk()'s on BPF side */
	skel->rodata->verbose = env.bpf_logs;
	skel->rodata->extra_verbose = env.debug_extra;
	skel->rodata->targ_tgid = env.pid;
	skel->rodata->emit_success_stacks = env.emit_success_stacks;
	skel->rodata->emit_intermediate_stacks = env.emit_intermediate_stacks;
	skel->rodata->duration_ns = env.longer_than_ms * 1000000ULL;

	memset(skel->rodata->spaces, ' ', 511);

	skel->rodata->use_ringbuf = env.has_ringbuf;
	if (env.has_ringbuf) {
		bpf_map__set_type(skel->maps.rb, BPF_MAP_TYPE_RINGBUF);
		bpf_map__set_key_size(skel->maps.rb, 0);
		bpf_map__set_value_size(skel->maps.rb, 0);
		bpf_map__set_max_entries(skel->maps.rb, env.ringbuf_sz);
	} else {
		bpf_map__set_type(skel->maps.rb, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
		bpf_map__set_key_size(skel->maps.rb, 4);
		bpf_map__set_value_size(skel->maps.rb, 4);
		bpf_map__set_max_entries(skel->maps.rb, 0);
	}

	/* LBR detection and setup */
	if (env.use_lbr && env.has_branch_snapshot) {
		lbr_perf_fds = malloc(sizeof(int) * env.cpu_cnt);
		if (!lbr_perf_fds) {
			err = -ENOMEM;
			goto cleanup;
		}
		for (i = 0; i < env.cpu_cnt; i++) {
			lbr_perf_fds[i] = -1;
		}

		err = create_lbr_perf_events(lbr_perf_fds, env.cpu_cnt);
		if (err) {
			if (env.verbose)
				fprintf(stderr, "Failed to create LBR perf events: %d. Disabling LBR capture.\n", err);
			err = 0;
		} else {
			env.has_lbr = true;
		}
	}
	env.use_lbr = env.use_lbr && env.has_lbr && env.has_branch_snapshot;
	skel->rodata->use_lbr = env.use_lbr;
	if (env.use_lbr && env.verbose)
		printf("LBR capture enabled.\n");

	att_opts.verbose = env.verbose;
	att_opts.debug = env.debug;
	att_opts.debug_extra = env.debug_extra;
	att_opts.dry_run = env.dry_run;
	switch (env.attach_mode) {
	case ATTACH_DEFAULT:
	case ATTACH_KPROBE_MULTI:
		att_opts.attach_mode = MASS_ATTACH_KPROBE;
		break;
	case ATTACH_KPROBE_SINGLE:
		att_opts.attach_mode = MASS_ATTACH_KPROBE_SINGLE;
		break;
	case ATTACH_FENTRY:
		att_opts.attach_mode = MASS_ATTACH_FENTRY;
		break;
	default:
		fprintf(stderr, "Unrecognized attach mode: %d.\n", env.attach_mode);
		err = -EINVAL;
		goto cleanup;
	}
	att_opts.func_filter = func_filter;
	att = mass_attacher__new(skel, &att_opts);
	if (!att)
		goto cleanup;

	/* entry globs are allow globs as well */
	for (i = 0; i < env.entry_glob_cnt; i++) {
		err = mass_attacher__allow_glob(att, env.entry_globs[i]);
		if (err)
			goto cleanup;
	}
	for (i = 0; i < env.allow_glob_cnt; i++) {
		err = mass_attacher__allow_glob(att, env.allow_globs[i]);
		if (err)
			goto cleanup;
	}
	for (i = 0; i < env.deny_glob_cnt; i++) {
		err = mass_attacher__deny_glob(att, env.deny_globs[i]);
		if (err)
			goto cleanup;
	}

	err = mass_attacher__prepare(att);
	if (err)
		goto cleanup;

	n = mass_attacher__func_cnt(att);
	if (n > MAX_FUNC_CNT) {
		fprintf(stderr,
			"Number of requested functions %d is too big, only up to %d functions are supported\n",
			n, MAX_FUNC_CNT);
		err = -E2BIG;
		goto cleanup;
	}

	vmlinux_btf = mass_attacher__btf(att);
	for (i = 0; i < n; i++) {
		const struct mass_attacher_func_info *finfo;
		const char *glob;
		__u32 flags;

		finfo = mass_attacher__func(att, i);
		flags = func_flags(finfo->name, vmlinux_btf, finfo->btf_id);

		for (j = 0; j < env.entry_glob_cnt; j++) {
			glob = env.entry_globs[j];
			if (!glob_matches(glob, finfo->name))
				continue;

			flags |= FUNC_IS_ENTRY;

			if (env.verbose)
				printf("Function '%s' is marked as an entry point.\n", finfo->name);

			break;
		}

		strncpy(skel->bss->func_names[i], finfo->name, MAX_FUNC_NAME_LEN - 1);
		skel->bss->func_names[i][MAX_FUNC_NAME_LEN - 1] = '\0';
		skel->bss->func_ips[i] = finfo->addr;
		skel->bss->func_flags[i] = flags;
	}

	for (i = 0; i < env.entry_glob_cnt; i++) {
		const char *glob = env.entry_globs[i];
		bool matched = false;

		for (j = 0, n = mass_attacher__func_cnt(att); j < n; j++) {
			const struct mass_attacher_func_info *finfo = mass_attacher__func(att, j);

			if (glob_matches(glob, finfo->name)) {
				matched = true;
				break;
			}
		}

		if (!matched) {
			err = -ENOENT;
			fprintf(stderr, "Entry glob '%s' doesn't match any kernel function!\n", glob);
			goto cleanup;
		}
	}

	err = mass_attacher__load(att);
	if (err)
		goto cleanup;

	for (i = 0; i < env.allow_pid_cnt; i++) {
		int tgid = env.allow_pids[i];
		bool verdict = true; /* allowed */

		err = bpf_map_update_elem(bpf_map__fd(skel->maps.tgids_filter),
					  &tgid, &verdict, BPF_ANY);
		if (err) {
			err = -errno;
			fprintf(stderr, "Failed to setup PID allowlist: %d\n", err);
			goto cleanup;
		}
	}
	/* denylist overrides allowlist, if overlaps */
	for (i = 0; i < env.deny_pid_cnt; i++) {
		int tgid = env.deny_pids[i];
		bool verdict = false; /* denied */

		err = bpf_map_update_elem(bpf_map__fd(skel->maps.tgids_filter),
					  &tgid, &verdict, BPF_ANY);
		if (err) {
			err = -errno;
			fprintf(stderr, "Failed to setup PID denylist: %d\n", err);
			goto cleanup;
		}
	}
	for (i = 0; i < env.allow_comm_cnt; i++) {
		const char *comm = env.allow_comms[i];
		char buf[TASK_COMM_LEN] = {};
		bool verdict = true; /* allowed */

		strncat(buf, comm, TASK_COMM_LEN - 1);

		err = bpf_map_update_elem(bpf_map__fd(skel->maps.comms_filter),
					  &buf, &verdict, BPF_ANY);
		if (err) {
			err = -errno;
			fprintf(stderr, "Failed to setup COMM allowlist: %d\n", err);
			goto cleanup;
		}
	}
	/* denylist overrides allowlist, if overlaps */
	for (i = 0; i < env.deny_comm_cnt; i++) {
		const char *comm = env.deny_comms[i];
		char buf[TASK_COMM_LEN] = {};
		bool verdict = false; /* denied */

		strncat(buf, comm, TASK_COMM_LEN - 1);

		err = bpf_map_update_elem(bpf_map__fd(skel->maps.comms_filter),
					  &buf, &verdict, BPF_ANY);
		if (err) {
			err = -errno;
			fprintf(stderr, "Failed to setup COMM denylist: %d\n", err);
			goto cleanup;
		}
	}

	ts1 = now_ns();
	err = mass_attacher__attach(att);
	if (err)
		goto cleanup;
	ts2 = now_ns();
	if (env.verbose)
		printf("Successfully attached in %ld ms.\n", (long)((ts2 - ts1) / 1000000));

	if (env.dry_run) {
		if (env.verbose)
			printf("Dry run successful, exiting...\n");
		goto cleanup_silent;
	}

	signal(SIGINT, sig_handler);

	env.ctx.att = att;
	env.ctx.ksyms = ksyms__load();
	if (!env.ctx.ksyms) {
		fprintf(stderr, "Failed to load /proc/kallsyms for symbolization.\n");
		goto cleanup;
	}

	/* Set up ring/perf buffer polling */
	if (env.has_ringbuf) {
		rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, &env.ctx, NULL);
		if (!rb) {
			err = -1;
			fprintf(stderr, "Failed to create ring buffer\n");
			goto cleanup;
		}
	} else {
		pb = perf_buffer__new(bpf_map__fd(skel->maps.rb),
				      env.perfbuf_percpu_sz / page_size,
				      handle_event_pb, NULL, &env.ctx, NULL);
		err = libbpf_get_error(pb);
		if (err) {
			fprintf(stderr, "Failed to create perf buffer: %d\n", err);
			goto cleanup;
		}
	}

	/* Allow mass tracing */
	mass_attacher__activate(att);

	/* Process events */
	if (env.bpf_logs)
		printf("BPF-side logging is enabled. Use `sudo cat /sys/kernel/debug/tracing/trace_pipe` to see logs.\n");
	printf("Receiving data...\n");
	while (!exiting) {
		err = rb ? ring_buffer__poll(rb, 100) : perf_buffer__poll(pb, 100);
		/* Ctrl-C will cause -EINTR */
		if (err == -EINTR) {
			err = 0;
			goto cleanup;
		}
		if (err < 0) {
			printf("Error polling perf buffer: %d\n", err);
			goto cleanup;
		}
	}

cleanup:
	printf("\nDetaching... ");
	ts1 = now_ns();
cleanup_silent:
	mass_attacher__free(att);

	addr2line__free(env.ctx.a2l);
	ksyms__free(env.ctx.ksyms);

	for (i = 0; i < env.cpu_cnt; i++) {
		if (lbr_perf_fds && lbr_perf_fds[i] >= 0)
			close(lbr_perf_fds[i]);
	}
	free(lbr_perf_fds);

	for (i = 0; i < env.allow_glob_cnt; i++)
		free(env.allow_globs[i]);
	free(env.allow_globs);
	for (i = 0; i < env.deny_glob_cnt; i++)
		free(env.deny_globs[i]);
	free(env.deny_globs);
	for (i = 0; i < env.entry_glob_cnt; i++)
		free(env.entry_globs[i]);
	free(env.entry_globs);

	for (i = 0; i < env.allow_comm_cnt; i++)
		free(env.allow_comms[i]);
	free(env.allow_comms);
	for (i = 0; i < env.deny_comm_cnt; i++)
		free(env.deny_comms[i]);
	free(env.deny_comms);

	free(env.allow_pids);
	free(env.deny_pids);

	ts2 = now_ns();
	printf("DONE in %ld ms.\n", (long)((ts2 - ts1) / 1000000));

	return -err;
}
