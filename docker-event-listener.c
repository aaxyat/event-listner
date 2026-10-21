/*
 * docker-event-listener.c — Extensible Docker Event Listener Daemon
 *
 * Connects directly to /var/run/docker.sock and monitors container lifecycle
 * and health events. Dispatches actions via:
 *   1. Config file rules (/docker/config/events.conf)
 *   2. Dynamic hooks directory (/docker/scripts/events.d/)
 *   3. Built-in defaults
 *
 * Footprint: < 500 KB RSS, 0.00% idle CPU.
 *
 * Author: Ayush Bhattarai (aaxyat)
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define DEFAULT_SOCKET_PATH    "/var/run/docker.sock"
#define DEFAULT_CONFIG_FILE    "/docker/config/events.conf"
#define DEFAULT_HOOKS_DIR      "/docker/scripts/events.d"
#define DEFAULT_COOLDOWN_SEC   15
#define MAX_RULES              64
#define MAX_LINE               1024

typedef struct {
    char container[64];
    char action[64];
    char command[512];
} EventRule;

static EventRule g_rules[MAX_RULES];
static int g_rule_count = 0;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_config = 0;
static int g_verbose = 0;

static void handle_signal(int sig) {
    if (sig == SIGHUP) {
        g_reload_config = 1;
    } else {
        g_running = 0;
    }
}

static void log_msg(const char *level, const char *msg) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    printf("[%s] [%s] %s\n", buf, level, msg);
    fflush(stdout);
}

static void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void load_config(const char *config_file) {
    g_rule_count = 0;
    FILE *f = fopen(config_file, "r");
    if (!f) {
        if (g_verbose) {
            char buf[256];
            snprintf(buf, sizeof(buf), "No config file at %s (using hooks directory & defaults)", config_file);
            log_msg("INFO", buf);
        }
        return;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '#' || line[0] == ';' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *target = line;
        char *cmd = eq + 1;
        trim(target);
        trim(cmd);

        char *colon = strchr(target, ':');
        if (!colon) continue;
        *colon = '\0';
        char *c_name = target;
        char *c_act = colon + 1;
        trim(c_name);
        trim(c_act);

        if (g_rule_count < MAX_RULES) {
            snprintf(g_rules[g_rule_count].container, sizeof(g_rules[g_rule_count].container), "%.63s", c_name);
            snprintf(g_rules[g_rule_count].action, sizeof(g_rules[g_rule_count].action), "%.63s", c_act);
            snprintf(g_rules[g_rule_count].command, sizeof(g_rules[g_rule_count].command), "%.511s", cmd);
            g_rule_count++;
        }
    }
    fclose(f);

    char buf[128];
    snprintf(buf, sizeof(buf), "Loaded %d rule(s) from %s", g_rule_count, config_file);
    log_msg("CONFIG", buf);
}

static int extract_field(const char *json, const char *key, char *out, size_t out_size) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":\"", key);
    const char *p = strstr(json, search_key);
    if (!p) return 0;
    p += strlen(search_key);
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len >= out_size) len = out_size - 1;
    strncpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int match_pattern(const char *pattern, const char *value) {
    if (strcmp(pattern, "*") == 0 || strcmp(pattern, value) == 0) return 1;
    if (strstr(value, pattern) != NULL) return 1;
    return 0;
}

static void execute_handler(const char *cmd_or_script, const char *container, const char *action, const char *id) {
    char log_buf[512];
    snprintf(log_buf, sizeof(log_buf), "Triggering action for %s (%s): %s", container, action, cmd_or_script);
    log_msg("ACTION", log_buf);

    pid_t pid = fork();
    if (pid == 0) {
        setenv("CONTAINER_NAME", container, 1);
        setenv("EVENT_ACTION", action, 1);
        setenv("CONTAINER_ID", id, 1);

        execl("/bin/sh", "sh", "-c", cmd_or_script, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            snprintf(log_buf, sizeof(log_buf), "Action for %s exited with code %d", container, WEXITSTATUS(status));
        } else {
            snprintf(log_buf, sizeof(log_buf), "Action for %s terminated abnormally", container);
        }
        log_msg("ACTION", log_buf);
    }
}

static void dispatch_event(const char *hooks_dir, const char *container, const char *action, const char *id) {
    // 1. Check configured rules
    for (int i = 0; i < g_rule_count; i++) {
        if (match_pattern(g_rules[i].container, container) &&
            match_pattern(g_rules[i].action, action)) {
            execute_handler(g_rules[i].command, container, action, id);
            return;
        }
    }

    // 2. Check dynamic hooks directory
    // Construct sanitized action string (e.g. "health_status: unhealthy" -> "unhealthy")
    const char *short_action = action;
    if (strncmp(action, "health_status: ", 15) == 0) {
        short_action = action + 15;
    }

    char hook_path[512];
    struct stat st;

    // Check <hooks_dir>/<container>_<action>.sh
    snprintf(hook_path, sizeof(hook_path), "%s/%s_%s.sh", hooks_dir, container, short_action);
    if (stat(hook_path, &st) == 0 && (st.st_mode & S_IXUSR)) {
        execute_handler(hook_path, container, action, id);
        return;
    }

    // Check <hooks_dir>/<container>.sh
    snprintf(hook_path, sizeof(hook_path), "%s/%s.sh", hooks_dir, container);
    if (stat(hook_path, &st) == 0 && (st.st_mode & S_IXUSR)) {
        execute_handler(hook_path, container, action, id);
        return;
    }

    // Check <hooks_dir>/default.sh
    snprintf(hook_path, sizeof(hook_path), "%s/default.sh", hooks_dir);
    if (stat(hook_path, &st) == 0 && (st.st_mode & S_IXUSR)) {
        execute_handler(hook_path, container, action, id);
        return;
    }

    // 3. Built-in fallback: gluetun unhealthy
    if (strcmp(container, "gluetun") == 0 && strstr(action, "unhealthy") != NULL) {
        const char *fallback = "/docker/scripts/gluetun-watchdog.sh";
        if (access(fallback, X_OK) == 0) {
            execute_handler(fallback, container, action, id);
            return;
        }
    }

    if (g_verbose) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Ignored unhandled event: %s on %s", action, container);
        log_msg("DEBUG", buf);
    }
}

int main(int argc, char *argv[]) {
    const char *socket_path = DEFAULT_SOCKET_PATH;
    const char *config_file = DEFAULT_CONFIG_FILE;
    const char *hooks_dir = DEFAULT_HOOKS_DIR;

    int opt;
    while ((opt = getopt(argc, argv, "s:c:d:vh")) != -1) {
        switch (opt) {
            case 's': socket_path = optarg; break;
            case 'c': config_file = optarg; break;
            case 'd': hooks_dir = optarg; break;
            case 'v': g_verbose = 1; break;
            case 'h':
                printf("Usage: %s [options]\n", argv[0]);
                printf("Options:\n");
                printf("  -s <path>    Docker UNIX socket path (default: %s)\n", DEFAULT_SOCKET_PATH);
                printf("  -c <file>    Rules configuration file (default: %s)\n", DEFAULT_CONFIG_FILE);
                printf("  -d <dir>     Dynamic hooks directory (default: %s)\n", DEFAULT_HOOKS_DIR);
                printf("  -v           Enable verbose / debug logging\n");
                printf("  -h           Show this help message\n");
                return 0;
            default:
                return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf), "Starting Docker Event Listener Daemon (socket: %s, hooks: %s)", socket_path, hooks_dir);
    log_msg("INIT", log_buf);

    load_config(config_file);

    time_t last_event_time = 0;
    char last_container[64] = {0};

    while (g_running) {
        if (g_reload_config) {
            g_reload_config = 0;
            load_config(config_file);
        }

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            log_msg("WARN", "Failed to create UNIX domain socket. Retrying in 3s...");
            sleep(3);
            continue;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            snprintf(log_buf, sizeof(log_buf), "Cannot connect to %s (%s). Retrying in 3s...", socket_path, strerror(errno));
            log_msg("WARN", log_buf);
            close(fd);
            sleep(3);
            continue;
        }

        // Stream all container-type events
        const char *req =
            "GET /events?filters=%7B%22type%22%3A%5B%22container%22%5D%7D HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n\r\n";

        if (write(fd, req, strlen(req)) < 0) {
            log_msg("WARN", "Failed to send HTTP request to Docker socket. Retrying in 3s...");
            close(fd);
            sleep(3);
            continue;
        }

        log_msg("INIT", "Connected to Docker event stream. Blocking on kernel epoll/recv...");

        FILE *stream = fdopen(fd, "r");
        if (!stream) {
            close(fd);
            sleep(3);
            continue;
        }

        char *line = NULL;
        size_t len = 0;
        ssize_t nread;

        while (g_running && (nread = getline(&line, &len, stream)) != -1) {
            if (g_reload_config) {
                g_reload_config = 0;
                load_config(config_file);
            }

            // Quick pre-filter: must be a JSON line with Action
            if (line[0] != '{' && strstr(line, "Action") == NULL) continue;

            char type[32] = {0};
            char action[64] = {0};
            char name[64] = {0};
            char id[64] = {0};

            extract_field(line, "Type", type, sizeof(type));
            if (strcmp(type, "container") != 0 && type[0] != '\0') continue;

            extract_field(line, "Action", action, sizeof(action));
            extract_field(line, "name", name, sizeof(name));
            extract_field(line, "ID", id, sizeof(id));

            if (name[0] == '\0' || action[0] == '\0') continue;

            // Only process significant lifecycle and health actions
            if (strstr(action, "health_status") != NULL ||
                strcmp(action, "die") == 0 ||
                strcmp(action, "oom") == 0 ||
                strcmp(action, "restart") == 0 ||
                strcmp(action, "stop") == 0) {

                // Flapping / duplicate cooldown filter per container
                time_t now = time(NULL);
                if (strcmp(name, last_container) == 0 && (now - last_event_time < DEFAULT_COOLDOWN_SEC)) {
                    if (g_verbose) {
                        snprintf(log_buf, sizeof(log_buf), "Cooldown active for %s (%s). Skipping.", name, action);
                        log_msg("DEBUG", log_buf);
                    }
                    continue;
                }
                last_event_time = now;
                snprintf(last_container, sizeof(last_container), "%.63s", name);

                snprintf(log_buf, sizeof(log_buf), "Event detected: %s on container '%s'", action, name);
                log_msg("EVENT", log_buf);

                dispatch_event(hooks_dir, name, action, id);
            }
        }

        free(line);
        fclose(stream);

        if (g_running) {
            log_msg("WARN", "Docker event stream disconnected. Reconnecting in 3s...");
            sleep(3);
        }
    }

    log_msg("INIT", "Daemon shutting down cleanly.");
    return 0;
}
