#include "cli.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include <mqtt.h>

#include "cynk_device.h"
#include "net.h"

#define CYNK_SEND_BUF_SIZE 2048
#define CYNK_RECV_BUF_SIZE 2048

enum cli_profile {
  PROFILE_SENSOR = 0,
  PROFILE_CONTROLLER = 1,
  PROFILE_HYBRID = 2,
  PROFILE_CUSTOM = 3
};

struct cli_state {
  struct cli_config cfg;
  struct mqtt_client mqtt;
  uint8_t sendbuf[CYNK_SEND_BUF_SIZE];
  uint8_t recvbuf[CYNK_RECV_BUF_SIZE];
  cynk_device *device;
  cynk_transport tx;
  cynk_device_config dev_cfg;
  cynk_mqtt_socket *sock;
  compat_thread_t worker;
  int worker_running;
  int worker_started;
  int connected;
  enum cli_profile profile;
  char last_topic[CYNK_TOPIC_MAX];
  size_t last_len;
  int last_qos;
};

static void cli_disconnect(struct cli_state *state);

struct cli_line_state {
  compat_mutex_t lock;
  int active;
  const char *prompt;
  char buf[2048];
  size_t len;
  size_t cursor;
};

static struct cli_line_state g_line = {
    COMPAT_MUTEX_INITIALIZER, 0, NULL, {0}, 0, 0};

static void cli_redraw_line_locked(void);
static void cli_async_begin(void);
static void cli_async_end(void);
static void cli_line_finish(int print_newline);

static const char *cli_prompt(void) {
  return "cynk-console> ";
}

static int cli_use_color(void) {
  const char *env = getenv("CYNK_CONSOLE_NO_COLOR");

  if (!compat_isatty(1)) {
    return 0;
  }

  if (env && env[0] == '1') {
    return 0;
  }

  return 1;
}

#define CYNK_CLR_RESET "\x1b[0m"
#define CYNK_CLR_DIM "\x1b[2m"
#define CYNK_CLR_RED "\x1b[31m"
#define CYNK_CLR_GREEN "\x1b[32m"
#define CYNK_CLR_YELLOW "\x1b[33m"
#define CYNK_CLR_BLUE "\x1b[34m"
#define CYNK_CLR_MAGENTA "\x1b[35m"
#define CYNK_CLR_CYAN "\x1b[36m"

static const char *cli_color(const char *code) {
  return cli_use_color() ? code : "";
}

static const char *cli_reset(void) {
  return cli_use_color() ? CYNK_CLR_RESET : "";
}

static void cli_print_prompt(void) {
  compat_mutex_lock(&g_line.lock);
  printf("%s%s%s", cli_color(CYNK_CLR_CYAN), cli_prompt(), cli_reset());
  fflush(stdout);
  compat_mutex_unlock(&g_line.lock);
}

struct cli_history {
  char **items;
  size_t len;
  size_t cap;
  char path[PATH_MAX];
};

static char *cli_skip_ws(char *p) {
  while (p && *p && isspace((unsigned char)*p)) {
    p++;
  }
  return p;
}

static void cli_trim_trailing(char *p) {
  size_t len = strlen(p);
  while (len > 0 && isspace((unsigned char)p[len - 1])) {
    p[len - 1] = '\0';
    len--;
  }
}

static char *cli_next_token(char **p) {
  char *start = cli_skip_ws(*p);
  char *end;

  if (!start || !*start) {
    *p = start;
    return NULL;
  }

  end = start;
  while (*end && !isspace((unsigned char)*end)) {
    end++;
  }
  if (*end) {
    *end = '\0';
    end++;
  }
  *p = end;
  return start;
}

static int cli_history_path(char *buf, size_t len) {
  const char *env = getenv("CYNK_CONSOLE_HISTORY");
  const char *home = compat_home_dir();
  const char *base = env && env[0] ? env : home;

  if (!base || !*base) {
    return -1;
  }

  if (env && env[0]) {
    if (snprintf(buf, len, "%s", env) >= (int)len) {
      return -1;
    }
    return 0;
  }

  if (snprintf(buf, len, "%s/.cynk-console_history", base) >= (int)len) {
    return -1;
  }

  return 0;
}

static void cli_history_push(struct cli_history *hist, const char *line) {
  char *copy;

  if (!line || !*line) {
    return;
  }

  if (hist->len > 0 && strcmp(hist->items[hist->len - 1], line) == 0) {
    return;
  }

  if (hist->len == hist->cap) {
    size_t next_cap = hist->cap ? hist->cap * 2 : 64;
    char **next_items = realloc(hist->items, next_cap * sizeof(char *));
    if (!next_items) {
      return;
    }
    hist->items = next_items;
    hist->cap = next_cap;
  }

  copy = strdup(line);
  if (!copy) {
    return;
  }

  hist->items[hist->len++] = copy;
}

static void cli_history_add(struct cli_history *hist, const char *line) {
  FILE *f;

  cli_history_push(hist, line);

  if (!hist->path[0]) {
    return;
  }

  f = fopen(hist->path, "a");
  if (!f) {
    return;
  }

  fprintf(f, "%s\n", line);
  fclose(f);
}

static void cli_history_init(struct cli_history *hist) {
  FILE *f;
  char line[2048];

  memset(hist, 0, sizeof(*hist));

  if (cli_history_path(hist->path, sizeof(hist->path)) != 0) {
    hist->path[0] = '\0';
    return;
  }

  f = fopen(hist->path, "r");
  if (!f) {
    return;
  }

  while (fgets(line, sizeof(line), f)) {
    cli_trim_trailing(line);
    cli_history_push(hist, line);
  }

  fclose(f);
}

static void cli_history_free(struct cli_history *hist) {
  size_t i;

  for (i = 0; i < hist->len; i++) {
    free(hist->items[i]);
  }
  free(hist->items);
  memset(hist, 0, sizeof(*hist));
}

static void cli_line_store_locked(const char *prompt, const char *buf, size_t len,
                                  size_t cursor) {
  size_t n = len;

  if (n >= sizeof(g_line.buf)) {
    n = sizeof(g_line.buf) - 1;
  }

  if (n > 0) {
    memcpy(g_line.buf, buf, n);
  }
  g_line.buf[n] = '\0';
  g_line.len = n;
  g_line.cursor = cursor > n ? n : cursor;
  g_line.prompt = prompt;
}

static void cli_redraw_line_locked(void) {
  const char *prompt = g_line.prompt ? g_line.prompt : "";

  printf("\r%s", prompt);
  if (g_line.len > 0) {
    fwrite(g_line.buf, 1, g_line.len, stdout);
  }
  fputs("\x1b[K", stdout);
  if (g_line.len > g_line.cursor) {
    printf("\x1b[%zuD", g_line.len - g_line.cursor);
  }
  fflush(stdout);
}

static void cli_redraw_line(const char *prompt, const char *buf, size_t len,
                            size_t cursor) {
  compat_mutex_lock(&g_line.lock);
  g_line.active = 1;
  cli_line_store_locked(prompt, buf, len, cursor);
  cli_redraw_line_locked();
  compat_mutex_unlock(&g_line.lock);
}

static void cli_print_suggestions(const char **items, size_t count) {
  size_t i;

  if (count == 0) {
    return;
  }

  compat_mutex_lock(&g_line.lock);
  fputs("\n", stdout);
  for (i = 0; i < count; i++) {
    printf("%s%s", items[i], (i + 1) % 4 == 0 || i + 1 == count ? "\n" : "\t");
  }
  fflush(stdout);
  compat_mutex_unlock(&g_line.lock);
}

static void cli_line_start(const char *prompt) {
  compat_mutex_lock(&g_line.lock);
  g_line.active = 1;
  g_line.prompt = prompt;
  g_line.len = 0;
  g_line.cursor = 0;
  g_line.buf[0] = '\0';
  fputs(prompt, stdout);
  fflush(stdout);
  compat_mutex_unlock(&g_line.lock);
}

static void cli_line_finish(int print_newline) {
  compat_mutex_lock(&g_line.lock);
  if (print_newline) {
    fputs("\n", stdout);
  }
  g_line.active = 0;
  g_line.prompt = NULL;
  g_line.len = 0;
  g_line.cursor = 0;
  g_line.buf[0] = '\0';
  fflush(stdout);
  compat_mutex_unlock(&g_line.lock);
}

static void cli_async_begin(void) {
  compat_mutex_lock(&g_line.lock);
  if (g_line.active) {
    fputs("\r\x1b[K", stdout);
  }
}

static void cli_async_end(void) {
  if (g_line.active) {
    cli_redraw_line_locked();
  } else {
    fflush(stdout);
  }
  compat_mutex_unlock(&g_line.lock);
}

static void cli_tab_complete(char *buf, size_t buflen, size_t *len, size_t *cursor) {
  static const char *cmds[] = {"connect", "send", "raw", "profile", "status",
                               "help", "quit", "exit"};
  static const char *profiles[] = {"sensor", "controller", "hybrid", "custom"};
  static const char *send_opts[] = {"slug=", "id="};
  const char **candidates = NULL;
  size_t candidates_len = 0;
  char cmd[32] = {0};
  size_t token_start;
  size_t token_len;
  size_t i;
  size_t cmd_len = 0;
  size_t match_count = 0;
  const char *match = NULL;
  const char *prompt = cli_prompt();

  if (*cursor > *len) {
    *cursor = *len;
  }

  token_start = *cursor;
  while (token_start > 0 && !isspace((unsigned char)buf[token_start - 1])) {
    token_start--;
  }
  token_len = *cursor - token_start;

  i = 0;
  while (i < *len && isspace((unsigned char)buf[i])) {
    i++;
  }
  while (i < *len && !isspace((unsigned char)buf[i]) && cmd_len + 1 < sizeof(cmd)) {
    cmd[cmd_len++] = buf[i++];
  }
  cmd[cmd_len] = '\0';

  if (token_start == i - cmd_len) {
    candidates = cmds;
    candidates_len = sizeof(cmds) / sizeof(cmds[0]);
  } else if (cmd_len > 0 && strcmp(cmd, "profile") == 0) {
    candidates = profiles;
    candidates_len = sizeof(profiles) / sizeof(profiles[0]);
  } else if (cmd_len > 0 && strcmp(cmd, "send") == 0) {
    candidates = send_opts;
    candidates_len = sizeof(send_opts) / sizeof(send_opts[0]);
  } else {
    return;
  }

  for (i = 0; i < candidates_len; i++) {
    if (token_len == 0 || strncmp(candidates[i], buf + token_start, token_len) == 0) {
      match = candidates[i];
      match_count++;
    }
  }

  if (match_count == 1 && match) {
    size_t match_len = strlen(match);
    if (match_len > token_len) {
      size_t extra = match_len - token_len;
      if (*len + extra + 1 >= buflen) {
        return;
      }
      memmove(buf + *cursor + extra, buf + *cursor, *len - *cursor + 1);
      memcpy(buf + *cursor, match + token_len, extra);
      *cursor += extra;
      *len += extra;
    }
    if (*cursor == *len && *len + 1 < buflen) {
      buf[*len] = ' ';
      buf[*len + 1] = '\0';
      (*len)++;
      (*cursor)++;
    }
    cli_redraw_line(prompt, buf, *len, *cursor);
    return;
  }

  if (match_count > 1) {
    const char *matches[16];
    size_t out = 0;

    for (i = 0; i < candidates_len && out < 16; i++) {
      if (token_len == 0 ||
          strncmp(candidates[i], buf + token_start, token_len) == 0) {
        matches[out++] = candidates[i];
      }
    }
    cli_print_suggestions(matches, out);
    cli_redraw_line(prompt, buf, *len, *cursor);
  }
}

static void cli_buffer_set(char *buf, size_t buflen, size_t *len,
                           size_t *cursor, const char *src) {
  size_t n = strlen(src);

  if (n >= buflen) {
    n = buflen - 1;
  }
  memcpy(buf, src, n);
  buf[n] = '\0';
  *len = n;
  *cursor = n;
}

static char *cli_readline(struct cli_history *hist, char *buf, size_t buflen) {
  compat_term_state term_state;
  int use_tty = compat_isatty(0);
  size_t len = 0;
  size_t cursor = 0;
  size_t hist_index = hist->len;
  char *saved = NULL;
  const char *prompt = cli_prompt();

  if (!use_tty) {
    cli_print_prompt();
    if (!fgets(buf, buflen, stdin)) {
      return NULL;
    }
    return buf;
  }

  if (compat_term_raw_enable(&term_state) != 0) {
    return NULL;
  }

  cli_line_start(prompt);

  for (;;) {
    unsigned char c;
    int n = compat_read_char(&c);

    if (n <= 0) {
      compat_term_raw_disable(&term_state);
      free(saved);
      cli_line_finish(0);
      return NULL;
    }

    if (c == '\r' || c == '\n') {
      buf[len] = '\0';
      cli_line_finish(1);
      compat_term_raw_disable(&term_state);
      free(saved);
      return buf;
    }

    if (c == '\t') {
      cli_tab_complete(buf, buflen, &len, &cursor);
      continue;
    }

    if (c == 3) {
      buf[0] = '\0';
      cli_line_finish(1);
      compat_term_raw_disable(&term_state);
      free(saved);
      return buf;
    }

    if (c == 4) {
      if (len == 0) {
        compat_term_raw_disable(&term_state);
        free(saved);
        cli_line_finish(0);
        return NULL;
      }
      continue;
    }

    if (c == 127 || c == 8) {
      if (cursor > 0) {
        memmove(buf + cursor - 1, buf + cursor, len - cursor);
        cursor--;
        len--;
        buf[len] = '\0';
        cli_redraw_line(prompt, buf, len, cursor);
      }
      continue;
    }

    if (c == 1) {
      cursor = 0;
      cli_redraw_line(prompt, buf, len, cursor);
      continue;
    }

    if (c == 5) {
      cursor = len;
      cli_redraw_line(prompt, buf, len, cursor);
      continue;
    }

    if (c == 27) {
      unsigned char seq[2];
      if (compat_read_char(&seq[0]) != 1) {
        continue;
      }
      if (compat_read_char(&seq[1]) != 1) {
        continue;
      }
      if (seq[0] != '[') {
        continue;
      }
      if (seq[1] == 'A') {
        if (hist->len == 0) {
          continue;
        }
        if (hist_index == hist->len) {
          free(saved);
          saved = strdup(buf);
        }
        if (hist_index > 0) {
          hist_index--;
          cli_buffer_set(buf, buflen, &len, &cursor,
                         hist->items[hist_index]);
          cli_redraw_line(prompt, buf, len, cursor);
        }
      } else if (seq[1] == 'B') {
        if (hist->len == 0) {
          continue;
        }
        if (hist_index < hist->len) {
          hist_index++;
          if (hist_index == hist->len) {
            cli_buffer_set(buf, buflen, &len, &cursor, saved ? saved : "");
          } else {
            cli_buffer_set(buf, buflen, &len, &cursor,
                           hist->items[hist_index]);
          }
          cli_redraw_line(prompt, buf, len, cursor);
        }
      } else if (seq[1] == 'C') {
        if (cursor < len) {
          cursor++;
          cli_redraw_line(prompt, buf, len, cursor);
        }
      } else if (seq[1] == 'D') {
        if (cursor > 0) {
          cursor--;
          cli_redraw_line(prompt, buf, len, cursor);
        }
      }
      continue;
    }

    if (isprint(c)) {
      if (len + 1 < buflen) {
        if (cursor < len) {
          memmove(buf + cursor + 1, buf + cursor, len - cursor);
          buf[cursor] = (char)c;
          len++;
          cursor++;
          buf[len] = '\0';
          cli_redraw_line(prompt, buf, len, cursor);
        } else {
          buf[len++] = (char)c;
          cursor = len;
          buf[len] = '\0';
          cli_redraw_line(prompt, buf, len, cursor);
        }
      }
    }
  }
}

static int cli_is_uuid(const char *s) {
  size_t i;

  if (!s || strlen(s) != 36) {
    return 0;
  }

  for (i = 0; i < 36; i++) {
    char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') {
        return 0;
      }
      continue;
    }
    if (!isxdigit((unsigned char)c)) {
      return 0;
    }
  }

  return 1;
}

static const char *profile_name(enum cli_profile profile) {
  switch (profile) {
  case PROFILE_SENSOR:
    return "sensor";
  case PROFILE_CONTROLLER:
    return "controller";
  case PROFILE_HYBRID:
    return "hybrid";
  case PROFILE_CUSTOM:
    return "custom";
  default:
    return "unknown";
  }
}

static const char *cynk_error_str(int rc) {
  switch (rc) {
  case CYNK_OK:
    return "ok";
  case CYNK_ERR_INVALID_ARG:
    return "invalid argument";
  case CYNK_ERR_NO_HANDSHAKE:
    return "handshake not ready";
  case CYNK_ERR_JSON:
    return "json parse error";
  case CYNK_ERR_TIMEOUT:
    return "timeout";
  case CYNK_ERR_NO_MEMORY:
    return "out of memory";
  case CYNK_ERR_PUBLISH:
    return "publish failed";
  case CYNK_ERR_SUBSCRIBE:
    return "subscribe failed";
  case CYNK_ERR_TIME:
    return "time error";
  case CYNK_ERR_BUFFER:
    return "buffer too small";
  default:
    return "unknown error";
  }
}

static uint64_t cli_now_ms(void *ctx) {
  (void)ctx;
  return compat_now_ms();
}

static int cli_now_iso8601(void *ctx, char *buf, size_t cap) {
  time_t now;
  struct tm tm;
  size_t written;

  (void)ctx;

  now = time(NULL);
  if (compat_gmtime(&now, &tm) != 0) {
    return -1;
  }

  written = strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return written > 0 ? 0 : -1;
}

static uint8_t qos_to_publish_flags(int qos) {
  switch (qos) {
  case 0:
    return MQTT_PUBLISH_QOS_0;
  case 2:
    return MQTT_PUBLISH_QOS_2;
  case 1:
  default:
    return MQTT_PUBLISH_QOS_1;
  }
}

static int cli_publish(void *ctx, const char *topic, const void *payload,
                       size_t len, int qos, int retain) {
  struct cli_state *state = ctx;
  uint8_t flags = qos_to_publish_flags(qos);
  enum MQTTErrors err;

  if (retain) {
    flags |= MQTT_PUBLISH_RETAIN;
  }

  err = mqtt_publish(&state->mqtt, topic, payload, len, flags);
  if (err != MQTT_OK) {
    return -1;
  }

  state->last_len = len;
  state->last_qos = qos;
  snprintf(state->last_topic, sizeof(state->last_topic), "%s", topic);

  return 0;
}

static int cli_subscribe(void *ctx, const char *topic, int qos) {
  struct cli_state *state = ctx;
  enum MQTTErrors err = mqtt_subscribe(&state->mqtt, topic, qos);
  return err == MQTT_OK ? 0 : -1;
}

static void cli_print_help(void) {
  printf("%sCommands:%s\n", cli_color(CYNK_CLR_BLUE), cli_reset());
  puts("  connect               connect to broker and run handshake");
  puts("  send <slug> <v>       send telemetry value by slug");
  puts("  send <id> <v>         send telemetry value by id (uuid only)");
  puts("  send slug=<slug> <v>  send telemetry value by slug");
  puts("  send id=<id> <v>      send telemetry value by id");
  puts("  send slug=<slug> value=<v>");
  puts("  raw <json>            send raw telemetry payload");
  puts("  profile <name>        set profile: sensor|controller|hybrid|custom");
  puts("  status                show connection and handshake status");
  puts("  help                  show this help");
  puts("  quit / exit           send offline status and exit");
}

static void cli_print_status(struct cli_state *state) {
  const char *user_id = cynk_device_user_id(state->device);
  int handshake_ready = cynk_device_handshake_ready(state->device);

  printf("Connection: %s%s%s\n",
         state->connected ? cli_color(CYNK_CLR_GREEN) : cli_color(CYNK_CLR_DIM),
         state->connected ? "connected" : "disconnected", cli_reset());
  printf("Profile: %s\n", profile_name(state->profile));
  printf("Handshake: %s%s%s\n",
         handshake_ready ? cli_color(CYNK_CLR_GREEN) : cli_color(CYNK_CLR_YELLOW),
         handshake_ready ? "ready" : "pending", cli_reset());
  if (user_id) {
    char telemetry[CYNK_TOPIC_MAX];
    snprintf(telemetry, sizeof(telemetry), "cynk/v1/%s/%s/telemetry", user_id,
             state->cfg.device_id);
    printf("User ID: %s%s%s\n", cli_color(CYNK_CLR_MAGENTA), user_id, cli_reset());
    printf("Telemetry topic: %s%s%s\n", cli_color(CYNK_CLR_DIM), telemetry,
           cli_reset());
  }
  if (state->last_topic[0]) {
    printf("Last publish: %stopic=%s%s bytes=%zu qos=%d\n", cli_color(CYNK_CLR_DIM),
           state->last_topic, cli_reset(), state->last_len, state->last_qos);
  }
}

static void cli_on_handshake(void *ctx, const char *user_id) {
  struct cli_state *state = ctx;
  char telemetry[CYNK_TOPIC_MAX];

  if (!user_id) {
    return;
  }

  snprintf(telemetry, sizeof(telemetry), "cynk/v1/%s/%s/telemetry", user_id,
           state->cfg.device_id);
  cli_async_begin();
  printf("%sHandshake complete.%s\n", cli_color(CYNK_CLR_GREEN), cli_reset());
  printf("User ID: %s%s%s\n", cli_color(CYNK_CLR_MAGENTA), user_id, cli_reset());
  printf("Telemetry topic: %s%s%s\n", cli_color(CYNK_CLR_DIM), telemetry,
         cli_reset());
  cli_async_end();
}

static void cli_on_command(void *ctx, const cynk_command *cmd) {
  struct cli_state *state = ctx;

  if (state->profile == PROFILE_SENSOR) {
    cli_async_begin();
    printf("%sCommand received, but current profile is sensor (ignoring).%s\n",
           cli_color(CYNK_CLR_YELLOW), cli_reset());
    cli_async_end();
    return;
  }

  cli_async_begin();
  printf("Command: %s%s%s\n", cli_color(CYNK_CLR_BLUE),
         cmd->command ? cmd->command : "(null)", cli_reset());
  if (cmd->request_id) {
    printf("Request ID: %s%s%s\n", cli_color(CYNK_CLR_MAGENTA),
           cmd->request_id, cli_reset());
  }
  if (cmd->widget.id || cmd->widget.slug) {
    printf("Widget:");
    if (cmd->widget.id) {
      printf(" id=%s", cmd->widget.id);
    }
    if (cmd->widget.slug) {
      printf(" slug=%s", cmd->widget.slug);
    }
    printf("\n");
  }
  if (cmd->params_json) {
    printf("Params: %s%s%s\n", cli_color(CYNK_CLR_DIM), cmd->params_json,
           cli_reset());
  }
  cli_async_end();
}

static void cli_publish_callback(void **state_ptr,
                                 struct mqtt_response_publish *published) {
  struct cli_state *state = *state_ptr;
  char stack_topic[CYNK_TOPIC_MAX];
  char *topic = stack_topic;
  size_t tlen = published->topic_name_size;

  if (!state || !state->device) {
    return;
  }

  if (tlen >= sizeof(stack_topic)) {
    topic = malloc(tlen + 1);
    if (!topic) {
      return;
    }
  }

  memcpy(topic, published->topic_name, tlen);
  topic[tlen] = '\0';

  cynk_device_handle_message(state->device, topic,
                             published->application_message,
                             published->application_message_size);

  if (topic != stack_topic) {
    free(topic);
  }
}

static void *cli_mqtt_worker(void *arg) {
  struct cli_state *state = arg;

  while (state->worker_running) {
    mqtt_sync(&state->mqtt);
    if (state->device) {
      int rc = cynk_device_poll(state->device);
      if (rc == CYNK_ERR_TIMEOUT) {
        cli_async_begin();
        fprintf(stderr, "%sHandshake timeout.%s\n", cli_color(CYNK_CLR_YELLOW),
                cli_reset());
        cli_async_end();
      }
    }
    if (state->mqtt.error != MQTT_OK) {
      cli_async_begin();
      fprintf(stderr, "%sMQTT error:%s %s\n", cli_color(CYNK_CLR_RED),
              cli_reset(), mqtt_error_str(state->mqtt.error));
      state->worker_running = 0;
      state->connected = 0;
      cli_async_end();
      break;
    }
    compat_sleep_ms(100);
  }

  return NULL;
}

static int cli_wait_for_connack(struct cli_state *state) {
  uint64_t start = cli_now_ms(NULL);

  for (;;) {
    struct mqtt_queued_message *msg;

    mqtt_sync(&state->mqtt);
    if (state->mqtt.error != MQTT_OK) {
      fprintf(stderr, "%sMQTT error:%s %s\n", cli_color(CYNK_CLR_RED),
              cli_reset(), mqtt_error_str(state->mqtt.error));
      return -1;
    }

    msg = mqtt_mq_find(&state->mqtt.mq, MQTT_CONTROL_CONNECT, NULL);
    if (!msg || msg->state == MQTT_QUEUED_COMPLETE) {
      return 0;
    }

    if (cli_now_ms(NULL) - start >= (uint64_t)state->cfg.handshake_timeout_ms) {
      fprintf(stderr, "%sConnect timeout.%s\n", cli_color(CYNK_CLR_YELLOW),
              cli_reset());
      return -1;
    }

    compat_sleep_ms(100);
  }
}

static int cli_wait_for_handshake(struct cli_state *state) {
  int rc;

  while (!cynk_device_handshake_ready(state->device) && state->worker_running) {
    rc = cynk_device_poll(state->device);
    if (rc == CYNK_ERR_TIMEOUT) {
      fprintf(stderr, "%sHandshake timeout.%s\n", cli_color(CYNK_CLR_YELLOW),
              cli_reset());
      return -1;
    }
    compat_sleep_ms(100);
  }

  return cynk_device_handshake_ready(state->device) ? 0 : -1;
}

static int cli_connect(struct cli_state *state) {
  cynk_net_config net_cfg;
  char err[192];
  char port_str[16];
  char will_payload[160];
  const char *will_topic;
  uint8_t flags = MQTT_CONNECT_CLEAN_SESSION | MQTT_CONNECT_WILL_QOS_1;
  int rc;

  if (state->connected) {
    printf("%sAlready connected.%s\n", cli_color(CYNK_CLR_YELLOW), cli_reset());
    return 0;
  }

  snprintf(port_str, sizeof(port_str), "%d", state->cfg.port);

  net_cfg.host = state->cfg.broker;
  net_cfg.port = port_str;
  net_cfg.ca_path = state->cfg.tls_ca;
  net_cfg.use_tls = state->cfg.tls;
  net_cfg.tls_insecure = state->cfg.tls_insecure;

  if (cynk_net_connect(&state->sock, &net_cfg, err, sizeof(err)) != 0) {
    fprintf(stderr, "%sConnection failed:%s %s\n", cli_color(CYNK_CLR_RED),
            cli_reset(), err);
    return -1;
  }

  mqtt_init(&state->mqtt, state->sock, state->sendbuf, sizeof(state->sendbuf),
            state->recvbuf, sizeof(state->recvbuf), cli_publish_callback);
  state->mqtt.publish_response_callback_state = state;

  rc = cynk_build_status_payload(state->device, "offline", will_payload,
                                 sizeof(will_payload));
  if (rc != CYNK_OK) {
    fprintf(stderr, "%sFailed to build offline payload:%s %s\n",
            cli_color(CYNK_CLR_RED), cli_reset(), cynk_error_str(rc));
    cynk_net_close(state->sock);
    state->sock = NULL;
    return -1;
  }

  will_topic = cynk_device_status_topic(state->device);

  mqtt_connect(&state->mqtt, state->cfg.device_id, will_topic, will_payload,
               strlen(will_payload), state->cfg.device_id, state->cfg.password,
               flags, (uint16_t)state->cfg.keepalive);
  if (state->mqtt.error != MQTT_OK) {
    fprintf(stderr, "%sMQTT connect error:%s %s\n", cli_color(CYNK_CLR_RED),
            cli_reset(), mqtt_error_str(state->mqtt.error));
    cynk_net_close(state->sock);
    state->sock = NULL;
    return -1;
  }

  if (cli_wait_for_connack(state) != 0) {
    cynk_net_close(state->sock);
    state->sock = NULL;
    return -1;
  }

  rc = cynk_device_on_connect(state->device);
  if (rc != CYNK_OK) {
    fprintf(stderr, "%sHandshake start failed:%s %s\n", cli_color(CYNK_CLR_RED),
            cli_reset(), cynk_error_str(rc));
    cli_disconnect(state);
    return -1;
  }

  state->worker_running = 1;
  if (compat_thread_create(&state->worker, cli_mqtt_worker, state) != 0) {
    fprintf(stderr, "%sFailed to start MQTT worker.%s\n",
            cli_color(CYNK_CLR_RED), cli_reset());
    state->worker_running = 0;
    cynk_net_close(state->sock);
    state->sock = NULL;
    return -1;
  }
  state->worker_started = 1;

  state->connected = 1;

  printf("%sConnected%s to %s:%d. Waiting for handshake...\n",
         cli_color(CYNK_CLR_GREEN), cli_reset(), state->cfg.broker,
         state->cfg.port);

  if (cli_wait_for_handshake(state) != 0) {
    cli_disconnect(state);
    return -1;
  }

  return 0;
}

static void cli_disconnect(struct cli_state *state) {
  char payload[160];
  int rc;
  int was_connected = state->connected;

  if (state->worker_started) {
    state->worker_running = 0;
    compat_thread_join(state->worker);
    state->worker_started = 0;
  }

  if (was_connected) {
    rc = cynk_build_status_payload(state->device, "offline", payload,
                                   sizeof(payload));
    if (rc == CYNK_OK) {
      mqtt_publish(&state->mqtt, cynk_device_status_topic(state->device), payload,
                   strlen(payload), MQTT_PUBLISH_QOS_1);
      mqtt_sync(&state->mqtt);
    }

    mqtt_disconnect(&state->mqtt);
    mqtt_sync(&state->mqtt);
  }

  if (state->sock) {
    cynk_net_close(state->sock);
    state->sock = NULL;
  }
  state->connected = 0;
}

static int cli_parse_value(char *input, cynk_value *value) {
  char *p = cli_skip_ws(input);
  char *end = NULL;
  double number;

  if (!p || !*p) {
    return -1;
  }

  cli_trim_trailing(p);

  if (strcmp(p, "true") == 0 || strcmp(p, "false") == 0) {
    value->type = CYNK_VALUE_BOOL;
    value->boolean = strcmp(p, "true") == 0;
    return 0;
  }

  if (strcmp(p, "null") == 0) {
    value->type = CYNK_VALUE_JSON;
    value->json = p;
    return 0;
  }

  if (p[0] == '{' || p[0] == '[') {
    value->type = CYNK_VALUE_JSON;
    value->json = p;
    return 0;
  }

  if (p[0] == '"' && p[strlen(p) - 1] == '"' && strlen(p) >= 2) {
    p[strlen(p) - 1] = '\0';
    value->type = CYNK_VALUE_STRING;
    value->string = p + 1;
    return 0;
  }

  number = strtod(p, &end);
  if (end && *cli_skip_ws(end) == '\0') {
    value->type = CYNK_VALUE_NUMBER;
    value->number = number;
    return 0;
  }

  value->type = CYNK_VALUE_STRING;
  value->string = p;
  return 0;
}

static void cli_send_value(struct cli_state *state, char *ref_token, char *value_str) {
  cynk_widget_ref ref = {0};
  cynk_value value = {0};
  int rc;

  if (!state->connected) {
    printf("%sNot connected.%s\n", cli_color(CYNK_CLR_YELLOW), cli_reset());
    return;
  }

  if (!ref_token) {
    printf("%sSend requires a slug or id.%s\n", cli_color(CYNK_CLR_YELLOW),
           cli_reset());
    return;
  }

  if (strncmp(ref_token, "slug=", 5) == 0) {
    ref.slug = ref_token + 5;
  } else if (strncmp(ref_token, "id=", 3) == 0) {
    ref.id = ref_token + 3;
  } else if (strncmp(ref_token, "value=", 6) == 0) {
    printf("%sSend requires a slug or id.%s\n", cli_color(CYNK_CLR_YELLOW),
           cli_reset());
    return;
  } else {
    if (cli_is_uuid(ref_token)) {
      ref.id = ref_token;
    } else {
      ref.slug = ref_token;
    }
  }

  if ((ref.slug && ref.slug[0] == '\0') || (ref.id && ref.id[0] == '\0')) {
    printf("%sWidget reference cannot be empty.%s\n",
           cli_color(CYNK_CLR_YELLOW), cli_reset());
    return;
  }

  if (ref.id && !cli_is_uuid(ref.id)) {
    printf("%sID must be a UUID.%s\n", cli_color(CYNK_CLR_YELLOW), cli_reset());
    return;
  }

  if (!value_str || !*value_str) {
    printf("%sSend requires a value.%s\n", cli_color(CYNK_CLR_YELLOW),
           cli_reset());
    return;
  }

  if (strncmp(value_str, "value=", 6) == 0) {
    value_str += 6;
    if (!*value_str) {
      printf("%sSend requires a value.%s\n", cli_color(CYNK_CLR_YELLOW),
             cli_reset());
      return;
    }
  }

  if (cli_parse_value(value_str, &value) != 0) {
    printf("%sInvalid value.%s\n", cli_color(CYNK_CLR_YELLOW), cli_reset());
    return;
  }

  rc = cynk_device_send_value(state->device, ref, value);
  if (rc != CYNK_OK) {
    fprintf(stderr, "%sSend failed:%s %s\n", cli_color(CYNK_CLR_RED),
            cli_reset(), cynk_error_str(rc));
    return;
  }

  printf("%sSent:%s topic=%s bytes=%zu qos=%d\n", cli_color(CYNK_CLR_GREEN),
         cli_reset(), state->last_topic, state->last_len, state->last_qos);
}

static void cli_send_raw(struct cli_state *state, char *payload) {
  int rc;

  if (!state->connected) {
    printf("%sNot connected.%s\n", cli_color(CYNK_CLR_YELLOW), cli_reset());
    return;
  }

  if (!payload || !*payload) {
    printf("%sRaw requires a JSON payload.%s\n", cli_color(CYNK_CLR_YELLOW),
           cli_reset());
    return;
  }

  rc = cynk_device_send_raw(state->device, payload, strlen(payload));
  if (rc != CYNK_OK) {
    fprintf(stderr, "%sRaw send failed:%s %s\n", cli_color(CYNK_CLR_RED),
            cli_reset(), cynk_error_str(rc));
    return;
  }

  printf("%sSent:%s topic=%s bytes=%zu qos=%d\n", cli_color(CYNK_CLR_GREEN),
         cli_reset(), state->last_topic, state->last_len, state->last_qos);
}

static void cli_set_profile(struct cli_state *state, const char *name) {
  if (!name) {
    printf("%sProfile requires a name.%s\n", cli_color(CYNK_CLR_YELLOW),
           cli_reset());
    return;
  }

  if (strcmp(name, "sensor") == 0) {
    state->profile = PROFILE_SENSOR;
  } else if (strcmp(name, "controller") == 0) {
    state->profile = PROFILE_CONTROLLER;
  } else if (strcmp(name, "hybrid") == 0) {
    state->profile = PROFILE_HYBRID;
  } else if (strcmp(name, "custom") == 0) {
    state->profile = PROFILE_CUSTOM;
  } else {
    printf("%sUnknown profile.%s\n", cli_color(CYNK_CLR_YELLOW), cli_reset());
    return;
  }

  printf("%sProfile set to %s.%s\n", cli_color(CYNK_CLR_GREEN),
         profile_name(state->profile), cli_reset());
}

int cli_run(const struct cli_config *cfg) {
  struct cli_state state;
  struct cli_history history;
  char line[2048];

  memset(&state, 0, sizeof(state));
  state.cfg = *cfg;
  state.profile = PROFILE_HYBRID;

  compat_console_init();

  state.tx.publish = cli_publish;
  state.tx.subscribe = cli_subscribe;
  state.tx.ctx = &state;

  state.dev_cfg.device_id = cfg->device_id;
  state.dev_cfg.handshake_timeout_ms = (uint32_t)cfg->handshake_timeout_ms;
  state.dev_cfg.qos = cfg->qos;
  state.dev_cfg.now_ms = cli_now_ms;
  state.dev_cfg.now_iso8601 = cli_now_iso8601;

  state.device = cynk_device_create(&state.dev_cfg, &state.tx);
  if (!state.device) {
    fprintf(stderr, "failed to initialize device\n");
    return 1;
  }

  cynk_device_set_command_cb(state.device, cli_on_command, &state);
  cynk_device_set_handshake_cb(state.device, cli_on_handshake, &state);

  if (cfg->tls) {
    if (cfg->tls_insecure)
      printf("TLS: %s:%d (insecure, no CA verification)\n", cfg->broker, cfg->port);
    else if (cfg->tls_ca)
      printf("TLS: %s:%d (CA: %s)\n", cfg->broker, cfg->port, cfg->tls_ca);
    else
      printf("TLS: %s:%d (warning: no CA certificate found)\n", cfg->broker, cfg->port);
  } else {
    printf("Connecting: %s:%d (no TLS)\n", cfg->broker, cfg->port);
  }

  puts("cynk-console shell ready. type 'help' for commands.");
  cli_history_init(&history);

  while (cli_readline(&history, line, sizeof(line))) {
    char original[2048];
    char *cursor = line;
    char *cmd;

    cli_trim_trailing(cursor);
    snprintf(original, sizeof(original), "%s", cursor);
    cmd = cli_next_token(&cursor);
    if (!cmd) {
      continue;
    }

    cli_history_add(&history, original);

    if (strcmp(cmd, "help") == 0) {
      cli_print_help();
    } else if (strcmp(cmd, "connect") == 0) {
      cli_connect(&state);
    } else if (strcmp(cmd, "status") == 0) {
      cli_print_status(&state);
    } else if (strcmp(cmd, "profile") == 0) {
      char *name = cli_next_token(&cursor);
      cli_set_profile(&state, name);
    } else if (strcmp(cmd, "send") == 0) {
      char *ref = cli_next_token(&cursor);
      char *value = cli_skip_ws(cursor);
      cli_send_value(&state, ref, value);
    } else if (strcmp(cmd, "raw") == 0) {
      char *payload = cli_skip_ws(cursor);
      cli_send_raw(&state, payload);
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
      break;
    } else {
      printf("%sUnknown command.%s\n", cli_color(CYNK_CLR_YELLOW), cli_reset());
    }
  }

  cli_disconnect(&state);
  cynk_device_destroy(state.device);
  cli_history_free(&history);
  printf("%sGoodbye.%s\n", cli_color(CYNK_CLR_DIM), cli_reset());
  return 0;
}
