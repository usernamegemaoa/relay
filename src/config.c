#include "config.h"

#include <errno.h>
#include <libgen.h>
#include <stdarg.h>
#include <string.h>

#include "log.h"
#include "string_util.h"
#include "worker.h"

static const char *OUR_NAME = "event-relay";

/* Global, boo, hiss. */
config_t CONFIG;

void config_destroy(void)
{
    int i;
    for (i = 0; i < CONFIG.argc; i++)
	free(CONFIG.argv[i]);
    free(CONFIG.argv);
    free(CONFIG.graphite.addr);
    free(CONFIG.graphite.target);
    free(CONFIG.spillway_root);
    free(CONFIG.file);
}

void config_set_defaults(config_t * config)
{
    if (config == NULL)
	return;

    config->syslog_to_stderr = DEFAULT_SYSLOG_TO_STDERR;
    config->tcp_send_timeout_sec = DEFAULT_TCP_SEND_TIMEOUT_SEC;
    config->polling_interval_millisec = DEFAULT_POLLING_INTERVAL_MILLISEC;
    config->sleep_after_disaster_millisec = DEFAULT_SLEEP_AFTER_DISASTER_MILLISEC;
    config->server_socket_rcvbuf_bytes = DEFAULT_SERVER_SOCKET_RCVBUF_BYTES;

    config->spill_usec = DEFAULT_SPILL_USEC;
    config->spillway_root = strdup(DEFAULT_SPILLWAY_ROOT);

    config->graphite.addr = strdup(DEFAULT_GRAPHITE_ADDR);
    config->graphite.target = strdup(DEFAULT_GRAPHITE_TARGET);
    config->graphite.send_interval_millisec = DEFAULT_GRAPHITE_SEND_INTERVAL_MILLISEC;
    config->graphite.sleep_poll_interval_millisec = DEFAULT_GRAPHITE_SLEEP_POLL_INTERVAL_MILLISEC;
}

void config_dump(config_t * config)
{
    if (config == NULL)
	return;
    SAY("config->syslog_to_stderr = %d", config->syslog_to_stderr);
    SAY("config->tcp_send_timeout_sec = %d", config->tcp_send_timeout_sec);
    SAY("config->polling_interval_millisec = %d", config->polling_interval_millisec);
    SAY("config->sleep_after_disaster_millisec = %d", config->sleep_after_disaster_millisec);
    SAY("config->server_socket_rcvbuf_bytes = %d", config->server_socket_rcvbuf_bytes);


    SAY("config->spillway_root = %s", config->spillway_root);
    SAY("config->spill_usec = %d", config->spill_usec);

    SAY("config->graphite.addr = %s", config->graphite.addr);
    SAY("config->graphite.target = %s", config->graphite.target);
    SAY("config->graphite.send_interval_millisec = %d", config->graphite.send_interval_millisec);
    SAY("config->graphite.sleep_poll_interval_millisec = %d", config->graphite.sleep_poll_interval_millisec);
    if (config->argc > 0)
	SAY("listener address = %s", config->argv[0]);
    for (int i = 1; i < config->argc; i++)
	SAY("forward address = %s", config->argv[i]);
}

static int is_non_empty_string(const char *s)
{
    return s && *s ? 1 : 0;
}

/* Accepts only ASCII paths: one or more 'words',
 * separated by single dots. */
static int is_valid_graphite_target(const char *path)
{
    if (!is_non_empty_string(path))
	return 0;
    const char *p;
    /* XXX maybe stricter: can the words start with a digit? */
    for (p = path; *p && (isalnum(*p) || *p == '_'); p++) {
	while (isalnum(*p) || *p == '_')
	    p++;
	if (*p == 0)
	    break;
	if (*p == '.' && isalnum(p[1]))
	    continue;
	else if (*p) {
	    return 0;
	}
    }
    return *p == 0;
}

static int is_valid_socketize(const char *arg, int default_proto, int connection_direction, const char *role)
{
    if (!is_non_empty_string(arg))
	return 0;
    /* NOTE: the result socketization is "lost" (beyond the success/failure)
     * and redone later when the listener and workers are started.  This may
     * be considered wasteful, but would get tricky on e.g. config reloads. */
    sock_t s;
    if (!socketize(arg, &s, default_proto, connection_direction, role))
	return 0;
    return 1;
}

static int is_valid_directory(const char *path)
{
    if (!is_non_empty_string(path))
	return 0;
    struct stat st;
    /* Yes, there's a race condition here. */
    return (stat(path, &st) == 0 || S_ISDIR(st.st_mode)) ? 1 : 0;
}

static int is_valid_millisec(uint32_t millisec)
{
    /* The upper limit is because of use of usleep():
     * 1000000 (1 sec) is promised by standards, but no more. */
    return millisec > 0 && millisec <= 1000000;
}

static int is_valid_microsec(uint32_t microsec)
{
    /* The upper limit is because of use of usleep():
     * 1000000 (1 sec) is promised by standards, but no more. */
    return microsec > 0 && microsec <= 1000000;
}

static int is_valid_sec(uint32_t sec)
{
    /* The upper limit is pretty arbitrary, but the basic idea is to
     * protect against too high values which indicate either mixing
     * with milliseconds, or overflows/wraparounds. */
    return sec > 0 && sec <= 60;
}

static int is_valid_buffer_size(uint32_t size)
{
    /* Pretty arbitrary choices but let's require alignment by 4096,
     * and at least one megabyte. */
    return ((size & 4095) == 0) && (size >= 1 << 20);
}

#define CONFIG_VALID_STR(config, t, v, invalid)		\
    do { if (!t(config->v)) { SAY("%s value '%s' invalid", #v, config->v); invalid++; } } while (0)

#define CONFIG_VALID_SOCKETIZE(config, p, d, r, v, invalid)		\
    do { if (!is_valid_socketize(config->v, p, d, r " (config check)")) { SAY("%s value '%s' invalid", #v, config->v); invalid++; } } while (0)

#define CONFIG_VALID_NUM(config, t, v, invalid)		\
    do { if (!t(config->v)) { SAY("%s value %d invalid", #v, config->v); invalid++; } } while (0)

static int config_valid(config_t * config)
{
    int invalid = 0;

    CONFIG_VALID_NUM(config, is_valid_sec, tcp_send_timeout_sec, invalid);
    CONFIG_VALID_NUM(config, is_valid_millisec, polling_interval_millisec, invalid);
    CONFIG_VALID_NUM(config, is_valid_millisec, sleep_after_disaster_millisec, invalid);
    CONFIG_VALID_NUM(config, is_valid_buffer_size, server_socket_rcvbuf_bytes, invalid);

    CONFIG_VALID_STR(config, is_valid_directory, spillway_root, invalid);
    CONFIG_VALID_NUM(config, is_valid_microsec, spill_usec, invalid);

    CONFIG_VALID_SOCKETIZE(config, IPPROTO_TCP, RELAY_CONN_IS_OUTBOUND, "graphite worker", graphite.addr, invalid);
    CONFIG_VALID_STR(config, is_valid_graphite_target, graphite.target, invalid);
    CONFIG_VALID_NUM(config, is_valid_millisec, graphite.send_interval_millisec, invalid);
    CONFIG_VALID_NUM(config, is_valid_millisec, graphite.sleep_poll_interval_millisec, invalid);

    if (config->argc < 1) {
	SAY("Missing listener address");
	invalid++;
    } else {
	CONFIG_VALID_SOCKETIZE(config, IPPROTO_UDP, RELAY_CONN_IS_INBOUND, "listener", argv[0], invalid);
    }
    if (config->argc < 2) {
	SAY("Missing forward addresses");
	invalid++;
    } else {
	for (int i = 1; i < config->argc; i++) {
	    CONFIG_VALID_SOCKETIZE(config, IPPROTO_TCP, RELAY_CONN_IS_OUTBOUND, "forward", argv[i], invalid);
	}
    }

    return invalid == 0;
}

#define TRY_OPT_BEGIN do
#define TRY_OPT_END   while (0)

#define TRY_NUM_OPT(name,line,p)                                            \
    if ( STREQ(#name, line) ) {                                             \
        char* endp;                                                         \
        long tmp = strtol(p, &endp, 10);		                    \
        if (*endp == 0)                                                     \
             config->name = tmp;                                            \
        else     							    \
	    WARN("Ignoring strange config value %s=%s", #name, p);	    \
        break;                                                              \
    }

#define TRY_STR_OPT(name,line,p)                                            \
    if ( STREQ(#name, line) ) {                                             \
        free(config->name);                                                 \
        config->name = strdup(p);                                           \
        break;                                                              \
    }

static config_t *config_from_file(char *file)
{
    FILE *f;
    char *line = NULL;
    size_t len = 0;
    int line_num = 0;
    config_t *config = calloc_or_die(sizeof(config_t));

    config_set_defaults(config);

    if (file == NULL) {
	SAY("Config file unknown");
	return NULL;
    }
    SAY("Loading config file %s", file);
    f = fopen(file, "r");
    if (f == NULL) {
	SAY("Failed to open: %s (%s)", file, strerror(errno));
	return NULL;
    }

    while (getline(&line, &len, f) != -1) {
	char *p;

	line_num++;

	/* End-of-line comment. */
	if ((p = strchr(line, '#')))
	    *p = '\0';

	trim_space(line);

	if (*line) {
	    if ((p = strchr(line, '='))) {
		if (strlen(p) == 1) {
		    SAY("Error in config file %s:%d: %s", file, line_num, line);
		    return NULL;
		}
		*p = '\0';
		p++;
		TRY_OPT_BEGIN {
		    TRY_NUM_OPT(syslog_to_stderr, line, p);

		    TRY_NUM_OPT(tcp_send_timeout_sec, line, p);
		    TRY_NUM_OPT(polling_interval_millisec, line, p);
		    TRY_NUM_OPT(sleep_after_disaster_millisec, line, p);
		    TRY_NUM_OPT(server_socket_rcvbuf_bytes, line, p);

		    TRY_STR_OPT(spillway_root, line, p);
		    TRY_NUM_OPT(spill_usec, line, p);

		    TRY_STR_OPT(graphite.addr, line, p);
		    TRY_STR_OPT(graphite.target, line, p);
		    TRY_NUM_OPT(graphite.send_interval_millisec, line, p);
		    TRY_NUM_OPT(graphite.sleep_poll_interval_millisec, line, p);

		    SAY("Error in config file %s:%d: bad config option: %s", file, line_num, line);
		    return NULL;
		}
		TRY_OPT_END;

	    } else {
		config->argv = realloc_or_die(config->argv, sizeof(line) * (config->argc + 1));
		config->argv[config->argc] = strdup(line);
		config->argc++;
	    }
	}
    }
    fclose(f);
    if (line)
	free(line);
    SAY("Loaded config file %s", file);

    if (!config_valid(config)) {
	config_dump(config);
	SAY("Invalid configuration");
	return NULL;
    }

    return config;
}

/* TODO: this could be useful in graphite output */
struct bufferf {
    char buf[4096];
    int offset;
};

int append_to_bufferf(struct bufferf *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int room = sizeof(buf->buf) - buf->offset;
    int wrote = vsnprintf(buf->buf + buf->offset, room, fmt, ap);
    if (wrote < 0 || wrote >= room)
	return 0;
    buf->offset += wrote;
    return wrote;
}

static int config_to_bufferf(const config_t * config, struct bufferf *buf)
{
    if (!append_to_bufferf(buf, "syslog_to_stderr = %d\n", config->syslog_to_stderr))
	return 0;
    if (!append_to_bufferf(buf, "tcp_send_timeout_sec = %d\n", config->tcp_send_timeout_sec))
	return 0;
    if (!append_to_bufferf(buf, "spillway_root = %s\n", config->spillway_root))
	return 0;
    if (!append_to_bufferf(buf, "spill_usec = %d\n", config->spill_usec))
	return 0;
    if (!append_to_bufferf(buf, "polling_interval_millisec = %d\n", config->polling_interval_millisec))
	return 0;
    if (!append_to_bufferf(buf, "sleep_after_disaster_millisec = %d\n", config->sleep_after_disaster_millisec))
	return 0;
    if (!append_to_bufferf(buf, "server_socket_rcvbuf_bytes = %d\n", config->server_socket_rcvbuf_bytes))
	return 0;
    if (!append_to_bufferf(buf, "graphite.addr = %s\n", config->graphite.addr))
	return 0;
    if (!append_to_bufferf(buf, "graphite.target = %s\n", config->graphite.target))
	return 0;
    if (!append_to_bufferf(buf, "graphite.send_interval_millisec = %d\n", config->graphite.send_interval_millisec))
	return 0;
    if (!append_to_bufferf
	(buf, "graphite.sleep_poll_interval_millisec = %d\n", config->graphite.sleep_poll_interval_millisec))
	return 0;
    for (int i = 0; i < config->argc; i++) {
	if (!append_to_bufferf(buf, "%s\n", config->argv[i]))
	    return 0;
    }
    return 1;
}

static int config_to_file(const config_t * config, int fd)
{
    struct bufferf buf;

    memset(&buf, 0, sizeof(buf));
    if (config_to_bufferf(config, &buf)) {
	int wrote;
	if ((wrote = write(fd, buf.buf, buf.offset)) == buf.offset) {
	    return 1;
	} else {
	    WARN("write() failed, tried writing %d but wrote %d: %s", buf.offset, wrote, strerror(errno));
	}
    }
    WARN("Failed to write config to file");
    return 0;
}

static int config_save(const config_t * config, time_t now)
{
    if (config->file == NULL) {
	WARN("Failed to save config with NULL name");
	return 0;
    }

    /* We will do mkstemp() + rename(), but first we need a temp file
     * name template, and we need it in in the same directory as the
     * configuration file. */
    char temp[PATH_MAX];
    char *p = config->file;
    char *q = temp;
    char *qe = temp + sizeof(temp);
    /* Safer than strcpy or strncpy */
    while (*p && q < qe)
	*q++ = *p++;
    if (q < qe) {
	*q = 0;
    } else {
	WARN("Failed to copy config filename %s", config->file);
	return 0;
    }

    char *dir = dirname(temp);	/* NOTE: MODIFIES temp! */
    int len = strlen(dir);

    int wrote = snprintf(temp + len, sizeof(temp) - len, "/event-relay.conf.XXXXXX");
    if (wrote < 0 || wrote >= PATH_MAX) {
	WARN("Failed making filename %s: %s", temp, strerror(errno));
	return 0;
    }

    int fd = mkstemp(temp);
    if (fd == -1) {
	WARN("Failed to mkstemp %s: %s", temp, strerror(errno));
	return 0;
    }

    if (!config_to_file(config, fd)) {
	WARN("Failed to save config to %s: %s", temp, strerror(errno));
	return 0;
    }

    if (close(fd) == -1) {
	WARN("Failed to close save config as %s: %s", temp, strerror(errno));
	return 0;
    }

    char save[PATH_MAX];
    wrote = snprintf(save, PATH_MAX, "%s.save.%ld", config->file, now);
    if (wrote < 0 || wrote >= PATH_MAX) {
	WARN("Failed to write %s as %s.save (wrote %d bytes)", save, save, wrote);
	return 0;
    }

    if (rename(temp, save) != 0) {
	WARN("Failed to rename %s as %s (%s)", temp, save, strerror(errno));
	return 0;
    }

    SAY("Saved config as %s", save);
    return 1;
}

#define IF_NUM_OPT_CHANGED(name,config,new_config)          \
  do { \
    if ( config->name != new_config->name ) {               \
        SAY("Changed '" #name "' from '%d' to '%d'",        \
                config->name, new_config->name);            \
        config->name = new_config->name;                    \
        config_changed = 1;                                 \
    } \
  } while(0)

#define IF_STR_OPT_CHANGED(name,config,new_config)          \
  do { \
    if ( STRNE(config->name, new_config->name) )       {    \
        SAY("Changed '" #name "' from '%s' to '%s'",        \
                config->name, new_config->name);            \
        free(config->name);                                 \
        config->name = new_config->name;                    \
        config_changed = 1;                                 \
    } \
  } while(0)

int config_reload(config_t * config)
{
    time_t now = time(NULL);
    int config_changed = 0;

    config->epoch_attempt = now;

    SAY("Config reload start: generation %ld epoch_attempt %ld epoch_changed %ld epoch_success %ld now %ld",
	config->generation, config->epoch_attempt, config->epoch_changed, config->epoch_success, now);

    if (config->generation == 0) {
	SAY("Loading config file %s", config->file);
	config_changed = 1;
    } else
	SAY("Reloading config file %s", config->file);

    config_t *new_config = config_from_file(config->file);

    if (new_config == NULL) {
	if (config->generation) {
	    SAY("Failed to reload config, not restarting");
	    config_changed = 0;
	    goto out;
	} else {
	    /* This is the initial startup: if there's no config,
	     * we should just die. */
	    DIE_RC(EXIT_FAILURE, "Failed to load config, not starting");
	}
    }

    if (config->generation == 0) {
	SAY("Loaded config file %s", config->file);
	SAY("New config");
    } else {
	SAY("Reloaded config file %s", config->file);
	SAY("New unmerged config");
    }

    config_dump(new_config);
    if (!config_valid(new_config)) {
	SAY("Invalid new configuration, ignoring it");
	config_changed = 0;
	goto out;
    }
    SAY("Merging new configuration with old");

    if (config->syslog_to_stderr != new_config->syslog_to_stderr) {
	closelog();
	openlog(OUR_NAME,
		LOG_CONS | LOG_ODELAY | LOG_PID | (new_config->syslog_to_stderr ? LOG_PERROR : 0), OUR_FACILITY);
	if (config->generation == 0)
	    SAY("Setting 'syslog_to_stderr' to '%d'", new_config->syslog_to_stderr);
	else
	    SAY("Changing 'syslog_to_stderr' from '%d' to '%d'", config->syslog_to_stderr,
		new_config->syslog_to_stderr);
	config->syslog_to_stderr = new_config->syslog_to_stderr;
	config_changed = 1;
    }

    IF_NUM_OPT_CHANGED(syslog_to_stderr, config, new_config);
    IF_NUM_OPT_CHANGED(tcp_send_timeout_sec, config, new_config);
    IF_NUM_OPT_CHANGED(polling_interval_millisec, config, new_config);
    IF_NUM_OPT_CHANGED(sleep_after_disaster_millisec, config, new_config);
    IF_NUM_OPT_CHANGED(server_socket_rcvbuf_bytes, config, new_config);

    IF_STR_OPT_CHANGED(spillway_root, config, new_config);
    IF_NUM_OPT_CHANGED(spill_usec, config, new_config);

    IF_STR_OPT_CHANGED(graphite.addr, config, new_config);
    IF_STR_OPT_CHANGED(graphite.target, config, new_config);
    IF_NUM_OPT_CHANGED(graphite.send_interval_millisec, config, new_config);
    IF_NUM_OPT_CHANGED(graphite.sleep_poll_interval_millisec, config, new_config);

    for (int i = 0; i < config->argc; i++) {
	if (i < new_config->argc) {
	    if (STRNE(config->argv[i], new_config->argv[i])) {
		if (config->generation == 0) {
		    SAY("Setting %s socket config to '%s'", i == 0 ? "listen" : "forward", new_config->argv[i]);
		} else {
		    SAY("Changing %s socket config from '%s' to '%s'",
			i == 0 ? "listen" : "forward", config->argv[i], new_config->argv[i]);
		}
		config_changed = 1;
	    }
	} else {
	    SAY("Stopping forward socket to '%s'", config->argv[i]);
	    config_changed = 1;
	}
	free(config->argv[i]);
    }
    free(config->argv);
    for (int i = config->argc; i < new_config->argc; i++) {
	SAY("Setting %s socket config to '%s'", i == 0 ? "listen" : "forward", new_config->argv[i]);
	config_changed = 1;
    }
    config->argc = new_config->argc;
    config->argv = new_config->argv;

    if (config->generation && config_changed) {
	SAY("Merged new config");
	config_dump(config);
    }

    free(new_config);

    if (config_changed) {
	config->generation++;
	config->epoch_changed = now;
    }
    config->epoch_success = now;

    SAY("Config reload: success");

    if (!config_save(config, now)) {
	SAY("Warning: config save failed");
    }

  out:

    SAY("Config reload: generation %ld epoch_attempt %ld epoch_changed %ld epoch_success %ld now %ld",
	config->generation, config->epoch_attempt, config->epoch_changed, config->epoch_success, now);

    if (config_changed)
	SAY("Config changed: requires restart");
    else
	SAY("Config unchanged: does not require restart");

    return config_changed;
}


void config_init(int argc, char **argv)
{
    int i = 0;
    memset(&CONFIG, 0, sizeof(CONFIG));
    config_set_defaults(&CONFIG);
    openlog(OUR_NAME, LOG_CONS | LOG_ODELAY | LOG_PID | (CONFIG.syslog_to_stderr ? LOG_PERROR : 0), OUR_FACILITY);

    if (argc < 2) {
	config_die_args(argc, argv);
    } else if (argc == 2) {
	CONFIG.file = strdup(argv[1]);
	config_reload(&CONFIG);
    } else {
	CONFIG.argv = realloc_or_die(CONFIG.argv, sizeof(char *) * (argc));
	for (i = 0; i < argc - 1; i++) {
	    CONFIG.argv[i] = strdup(argv[i + 1]);
	}
	CONFIG.argc = i;
    }
}


void config_die_args(int argc, char **argv)
{
    (void) argc;
    /* XXX: fix me! */
    /* XXX: how?!!! */
    DIE_RC(EXIT_FAILURE,
	   "%s local-host:local-port tcp@remote-host:remote-port ...\n"
	   "or file with socket description like:\n"
	   "\tlocal-host:local-port\n" "\ttcp@remote-host:remote-port ...\n", argv[0]);
}
