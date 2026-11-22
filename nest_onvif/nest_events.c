#include "nest_events.h"
#include "../nest_rtsp/nest_motion.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

#define MAX_PULL_POINTS 8
#define EVENT_QUEUE_SIZE 16
#define POLL_MS 125
#define DEFAULT_LEASE_SEC 300
#define MAX_LEASE_SEC 3600
#define MAX_PULL_SEC 60
#define EVENT_ACTION EVENTS_NS "/EventPortType/"
#define PULL_ACTION EVENTS_NS "/PullPointSubscription/"
#define WSN_ACTION "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/"
#define RULE_TOPIC "RuleEngine/CellMotionDetector/Motion"
#define ALARM_TOPIC "VideoSource/MotionAlarm"
#define CONCRETE "http://docs.oasis-open.org/wsn/t-1/TopicExpression/Concrete"
#define CONCRETE_SET "http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet"
#define ITEM_FILTER "http://www.onvif.org/ver10/tev/messageContentFilter/ItemFilter"

typedef enum { RULE_MOTION, VIDEO_MOTION, TOPIC_COUNT } motion_topic_t;
#define EVENT_MASK(topic, active) (1u << ((topic) * 2 + (active)))
#define ALL_EVENTS ((1u << (TOPIC_COUNT * 2)) - 1)

typedef struct {
    time_t utc;
    bool active;
    bool initialized;
    motion_topic_t topic;
} notification_t;

typedef struct {
    char id[33];
    uint32_t expires;
    unsigned event_filter;
    notification_t queue[EVENT_QUEUE_SIZE];
    unsigned first;
    unsigned count;
    int pull_fd;
    uint32_t pull_deadline;
    unsigned pull_limit;
    char relates_to[192];
} subscription_t;

static subscription_t subscriptions[MAX_PULL_POINTS];
static bool motion_active;
static uint32_t next_sample;
static bool have_sample_time;

static uint32_t monotonic_ms(void) {
    static long hz;
    if (!hz) {
        hz = sysconf(_SC_CLK_TCK);
        if (hz <= 0) hz = 100;
    }
    struct tms cpu;
    return (uint32_t)((uint64_t)(uint32_t)times(&cpu) * 1000 / (unsigned long)hz);
}

static bool reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static void utc_format(time_t value, char buf[32]) {
    struct tm utc;
    gmtime_r(&value, &utc);
    strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

static void lease_times(const subscription_t *s, uint32_t now, char current[32],
                        char expiration[32]) {
    time_t wall = time(NULL);
    unsigned remaining = reached(now, s->expires) ? 0 : s->expires - now;
    utc_format(wall, current);
    utc_format(wall + (remaining + 999) / 1000, expiration);
}

/* printf callers pass fixed XML and bounded, XML-safe values. */
static bool event_reply(int fd, const char *action, const char *relates_to,
                        const char *body, bool fault) {
    static char xml[12800];
    int n = snprintf(xml, sizeof(xml),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsa=\"http://www.w3.org/2005/08/addressing\""
        " xmlns:tev=\"" EVENTS_NS "\""
        " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
        " xmlns:wsrf-r=\"http://docs.oasis-open.org/wsrf/r-2\""
        " xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\""
        " xmlns:tns1=\"http://www.onvif.org/ver10/topics\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:xs=\"http://www.w3.org/2001/XMLSchema\""
        " xmlns:ter=\"http://www.onvif.org/ver10/error\">"
        "<s:Header><wsa:Action>%s</wsa:Action>%s%s%s</s:Header>"
        "<s:Body>%s</s:Body></s:Envelope>",
        action, *relates_to ? "<wsa:RelatesTo>" : "", relates_to,
        *relates_to ? "</wsa:RelatesTo>" : "", body);
    if (n < 0 || (size_t)n >= sizeof(xml)) return false;
    char header[384];
    int hn = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: application/soap+xml; charset=utf-8; action=\"%s\"\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n",
        fault ? "500 Internal Server Error" : "200 OK", action, n);
    return hn > 0 && (size_t)hn < sizeof(header) &&
           onvif_send_all(fd, header, (size_t)hn) && onvif_send_all(fd, xml, (size_t)n);
}

static void event_fault(int fd, const char *id, const char *code, const char *reason) {
    char body[640];
    snprintf(body, sizeof(body),
        "<s:Fault><s:Code><s:Value>s:Sender</s:Value>"
        "<s:Subcode><s:Value>%s</s:Value></s:Subcode></s:Code>"
        "<s:Reason><s:Text xml:lang=\"en\">%s</s:Text></s:Reason></s:Fault>",
        code, reason);
    event_reply(fd, "http://www.w3.org/2005/08/addressing/fault", id, body, true);
}

static void enqueue(subscription_t *s, notification_t notification) {
    for (unsigned topic = 0; topic < TOPIC_COUNT; topic++) {
        if (!(s->event_filter & EVENT_MASK(topic, notification.active))) continue;
        if (s->count == EVENT_QUEUE_SIZE) {
            s->first = (s->first + 1) % EVENT_QUEUE_SIZE;
            s->count--;
        }
        notification.topic = (motion_topic_t)topic;
        s->queue[(s->first + s->count) % EVENT_QUEUE_SIZE] = notification;
        s->count++;
    }
}

static void read_motion(void) {
    int value = 0;
    long wall = 0;
    FILE *f = fopen(NEST_MOTION_PATH, "r");
    if (f) {
        if (fscanf(f, "%d %ld", &value, &wall) != 2) value = 0;
        fclose(f);
    }
    bool active = value == 1;
    if (active == motion_active) return;
    motion_active = active;
    notification_t event = {.utc = wall ? (time_t)wall : time(NULL), .active = active};
    for (unsigned i = 0; i < MAX_PULL_POINTS; i++) {
        if (subscriptions[i].id[0]) enqueue(&subscriptions[i], event);
    }
}

#define RULE_ITEMS \
    "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"" MOTION_SOURCE_TOKEN "\"/>" \
    "<tt:SimpleItem Name=\"VideoAnalyticsConfigurationToken\" Value=\"" MOTION_ANALYTICS_TOKEN "\"/>" \
    "<tt:SimpleItem Name=\"Rule\" Value=\"" MOTION_RULE "\"/>"
#define ALARM_ITEMS "<tt:SimpleItem Name=\"Source\" Value=\"" MOTION_VIDEO_TOKEN "\"/>"

static void respond_to_pull(subscription_t *s, uint32_t now) {
    static char body[10800];
    char current[32], end[32];
    lease_times(s, now, current, end);
    int size = snprintf(body, sizeof(body),
        "<tev:PullMessagesResponse><tev:CurrentTime>%s</tev:CurrentTime>"
        "<tev:TerminationTime>%s</tev:TerminationTime>", current, end);
    unsigned count = s->count < s->pull_limit ? s->count : s->pull_limit;
    for (unsigned i = 0; i < count; i++) {
        const notification_t *message = &s->queue[(s->first + i) % EVENT_QUEUE_SIZE];
        char utc[32];
        utc_format(message->utc, utc);
        bool alarm = message->topic == VIDEO_MOTION;
        int added = snprintf(body + size, sizeof(body) - (size_t)size,
            "<wsnt:NotificationMessage>"
            "<wsnt:Topic Dialect=\"" CONCRETE_SET "\">tns1:%s</wsnt:Topic>"
            "<wsnt:Message><tt:Message UtcTime=\"%s\" PropertyOperation=\"%s\">"
            "<tt:Source>%s</tt:Source><tt:Data><tt:SimpleItem Name=\"%s\" Value=\"%s\"/>"
            "</tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>",
            alarm ? ALARM_TOPIC : RULE_TOPIC, utc, message->initialized ? "Initialized" : "Changed",
            alarm ? ALARM_ITEMS : RULE_ITEMS, alarm ? "State" : "IsMotion",
            message->active ? "true" : "false");
        if (added < 0 || (size_t)added >= sizeof(body) - (size_t)size -
                                            sizeof("</tev:PullMessagesResponse>")) {
            count = i;
            break;
        }
        size += added;
    }
    snprintf(body + size, sizeof(body) - (size_t)size, "</tev:PullMessagesResponse>");
    if (event_reply(s->pull_fd, PULL_ACTION "PullMessagesResponse", s->relates_to, body, false)) {
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        char ip[INET_ADDRSTRLEN] = "";
        if (count && getpeername(s->pull_fd, (struct sockaddr *)&peer, &peer_size) == 0)
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        for (unsigned i = 0; i < count; i++) {
            const notification_t *event = &s->queue[(s->first + i) % EVENT_QUEUE_SIZE];
            if (!event->initialized)
                fprintf(stderr, "[events] Sent %s=%s to %s\n",
                        event->topic == VIDEO_MOTION ? "MotionAlarm" : "IsMotion",
                        event->active ? "true" : "false", ip);
        }
        s->first = (s->first + count) % EVENT_QUEUE_SIZE;
        s->count -= count;
    }
    close(s->pull_fd);
    s->pull_fd = -1;
}

static void expire_subscription(subscription_t *s) {
    if (s->pull_fd >= 0) {
        event_fault(s->pull_fd, s->relates_to, "wsrf-r:ResourceUnknownFault", "Subscription expired");
        close(s->pull_fd);
    }
    s->id[0] = '\0';
}

void events_poll(fd_set *fds, int *max_fd, struct timeval *timeout) {
    uint32_t now = monotonic_ms();
    if (!have_sample_time || reached(now, next_sample)) {
        read_motion();
        next_sample = now + POLL_MS;
        have_sample_time = true;
    }
    unsigned wait = next_sample - now;
    for (unsigned i = 0; i < MAX_PULL_POINTS; i++) {
        subscription_t *s = &subscriptions[i];
        if (!s->id[0]) continue;
        if (reached(now, s->expires)) {
            expire_subscription(s);
        } else if (s->pull_fd >= 0) {
            if (s->count || reached(now, s->pull_deadline)) {
                respond_to_pull(s, now);
            } else {
                unsigned until_pull = s->pull_deadline - now;
                unsigned until_expiry = s->expires - now;
                if (until_pull < wait) wait = until_pull;
                if (until_expiry < wait) wait = until_expiry;
                FD_SET(s->pull_fd, fds);
                if (s->pull_fd > *max_fd) *max_fd = s->pull_fd;
            }
        }
    }
    timeout->tv_sec = wait / 1000;
    timeout->tv_usec = (wait % 1000) * 1000;
}

void events_readable(const fd_set *fds) {
    for (unsigned i = 0; i < MAX_PULL_POINTS; i++) {
        subscription_t *s = &subscriptions[i];
        if (s->id[0] && s->pull_fd >= 0 && FD_ISSET(s->pull_fd, fds)) {
            close(s->pull_fd);
            s->pull_fd = -1;
        }
    }
}

void events_close(void) {
    for (unsigned i = 0; i < MAX_PULL_POINTS; i++) {
        subscription_t *s = &subscriptions[i];
        if (s->id[0] && s->pull_fd >= 0) close(s->pull_fd);
    }
}

static bool new_id(char id[33]) {
    unsigned char random[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, random, sizeof(random));
    close(fd);
    if (n != sizeof(random)) return false;
    for (unsigned i = 0; i < sizeof(random); i++) snprintf(id + i * 2, 3, "%02x", random[i]);
    return true;
}

static int duration_ms(const char *text, int max_ms) {
    if (strncmp(text, "PT", 2) != 0 || !text[2]) return -1;
    const char *p = text + 2;
    unsigned total = 0;
    char previous = 0;
    while (*p) {
        if (!isdigit((unsigned char)*p)) return -1;
        char *next;
        unsigned long value = strtoul(p, &next, 10);
        if (value > (unsigned)max_ms) value = (unsigned)max_ms;
        unsigned ms;
        if (*next == 'H' && !previous) {
            ms = 3600000;
        } else if (*next == 'M' && previous != 'M' && previous != 'S') {
            ms = 60000;
        } else if (*next == 'S' && !next[1]) {
            ms = 1000;
        } else if (*next == '.') {
            /* xs:duration permits a fractional seconds component. */
            const char *fraction = next + 1;
            int part = 0, place = 100;
            if (!isdigit((unsigned char)*fraction)) return -1;
            while (isdigit((unsigned char)*fraction)) {
                part += (*fraction++ - '0') * place;
                place /= 10;
            }
            if (strcmp(fraction, "S") != 0) return -1;
            uint64_t result = total + (uint64_t)value * 1000 + (unsigned)part;
            return result > (unsigned)max_ms ? max_ms : (int)result;
        } else {
            return -1;
        }
        uint64_t end = total + (uint64_t)value * ms;
        total = end > (unsigned)max_ms ? (unsigned)max_ms : (unsigned)end;
        previous = *next;
        p = next + 1;
    }
    return (int)total;
}

static int subscription_lease(const char *xml, const char *tag) {
    char value[80];
    if (!onvif_xml_get_tag(xml, tag, value, sizeof(value)) || !value[0])
        return DEFAULT_LEASE_SEC * 1000;
    if (value[0] == 'P') return duration_ms(value, MAX_LEASE_SEC * 1000);

    struct tm utc = {0};
    int year, month, consumed = 0;
    if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &utc.tm_mday,
               &utc.tm_hour, &utc.tm_min, &utc.tm_sec, &consumed) != 6 ||
        month < 1 || month > 12 || utc.tm_mday < 1 || utc.tm_mday > 31 ||
        utc.tm_hour < 0 || utc.tm_hour > 23 || utc.tm_min < 0 || utc.tm_min > 59 ||
        utc.tm_sec < 0 || utc.tm_sec > 60)
        return -1;
    const char *zone = value + consumed;
    if (*zone == '.') {
        while (isdigit((unsigned char)*++zone)) {}
    }
    if (strcmp(zone, "Z") != 0) return -1;
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    int64_t seconds = (int64_t)timegm(&utc) - time(NULL);
    if (seconds <= 0) return -1;
    return seconds > MAX_LEASE_SEC ? MAX_LEASE_SEC * 1000 : (int)seconds * 1000;
}

static unsigned filter_topics(char *filter) {
    unsigned mask = 0;
    for (char *item = strtok(filter, "|"); item; item = strtok(NULL, "|")) {
        while (isspace((unsigned char)*item)) item++;
        char *end = item + strlen(item);
        while (end > item && isspace((unsigned char)end[-1])) *--end = '\0';
        char *colon = strchr(item, ':');
        const char *path = colon ? colon + 1 : item;
        if (!strcmp(path, "*")) return ALL_EVENTS;
        if (!strcmp(path, RULE_TOPIC) || !strcmp(path, "RuleEngine//.") ||
            !strcmp(path, "RuleEngine/CellMotionDetector//.") ||
            !strcmp(path, "RuleEngine/CellMotionDetector/*"))
            mask |= EVENT_MASK(RULE_MOTION, false) | EVENT_MASK(RULE_MOTION, true);
        if (!strcmp(path, ALARM_TOPIC) || !strcmp(path, "VideoSource//.") ||
            !strcmp(path, "VideoSource/*"))
            mask |= EVENT_MASK(VIDEO_MOTION, false) | EVENT_MASK(VIDEO_MOTION, true);
    }
    return mask;
}

/* Evaluate the ONVIF ItemFilter XPath subset against the four possible events.
 * Attribute expressions select item rows. boolean() converts them to events. */
static const struct {
    const char *name;
    const char *value;
    unsigned events;
} filter_items[] = {
    {"VideoSourceConfigurationToken", MOTION_SOURCE_TOKEN, EVENT_MASK(RULE_MOTION, 0) | EVENT_MASK(RULE_MOTION, 1)},
    {"VideoAnalyticsConfigurationToken", MOTION_ANALYTICS_TOKEN, EVENT_MASK(RULE_MOTION, 0) | EVENT_MASK(RULE_MOTION, 1)},
    {"Rule", MOTION_RULE, EVENT_MASK(RULE_MOTION, 0) | EVENT_MASK(RULE_MOTION, 1)},
    {"IsMotion", "false", EVENT_MASK(RULE_MOTION, 0)},
    {"IsMotion", "true", EVENT_MASK(RULE_MOTION, 1)},
    {"Source", MOTION_VIDEO_TOKEN, EVENT_MASK(VIDEO_MOTION, 0) | EVENT_MASK(VIDEO_MOTION, 1)},
    {"State", "false", EVENT_MASK(VIDEO_MOTION, 0)},
    {"State", "true", EVENT_MASK(VIDEO_MOTION, 1)},
};

#define FILTER_ITEMS (sizeof(filter_items) / sizeof(filter_items[0]))
#define ALL_ITEMS ((1u << FILTER_ITEMS) - 1)

typedef struct {
    const char *p;
    unsigned depth;
    bool invalid;
} item_parser_t;

static bool take(item_parser_t *parser, const char *token) {
    while (isspace((unsigned char)*parser->p)) parser->p++;
    size_t len = strlen(token);
    if (strncmp(parser->p, token, len) != 0) return false;
    parser->p += len;
    return true;
}

static unsigned parse_filter_or(item_parser_t *parser, bool attribute);

static unsigned parse_filter_comparison(item_parser_t *parser) {
    bool name = take(parser, "@Name");
    if ((!name && !take(parser, "@Value")) || !take(parser, "=")) {
        parser->invalid = true;
        return 0;
    }
    while (isspace((unsigned char)*parser->p)) parser->p++;
    char quote = *parser->p;
    if (quote != '"' && quote != '\'') {
        parser->invalid = true;
        return 0;
    }
    const char *value = ++parser->p;
    const char *end = strchr(value, quote);
    if (!end) {
        parser->invalid = true;
        return 0;
    }
    parser->p = end + 1;
    unsigned matches = 0;
    for (unsigned i = 0; i < FILTER_ITEMS; i++) {
        const char *candidate = name ? filter_items[i].name : filter_items[i].value;
        if (strlen(candidate) == (size_t)(end - value) &&
            memcmp(candidate, value, (size_t)(end - value)) == 0)
            matches |= 1u << i;
    }
    return matches;
}

static unsigned parse_item_path(item_parser_t *parser) {
    bool element = false;
    if (take(parser, "//")) {
        const char *name = parser->p;
        while (*parser->p && (isalnum((unsigned char)*parser->p) || strchr("_-.:", *parser->p))) {
            if (*parser->p == ':') name = parser->p + 1;
            parser->p++;
        }
        size_t len = (size_t)(parser->p - name);
        if (len == 11 && memcmp(name, "ElementItem", len) == 0) element = true;
        else if (len != 10 || memcmp(name, "SimpleItem", len) != 0) parser->invalid = true;
    }
    if (!take(parser, "[")) {
        parser->invalid = true;
        return 0;
    }
    unsigned items = parse_filter_or(parser, true);
    if (!take(parser, "]") || !take(parser, ")")) parser->invalid = true;
    if (element) return 0;  /* The motion properties contain only SimpleItems. */

    unsigned events = 0;
    for (unsigned i = 0; i < FILTER_ITEMS; i++) {
        if (items & (1u << i)) events |= filter_items[i].events;
    }
    return events;
}

static unsigned parse_filter_atom(item_parser_t *parser, bool attribute) {
    if (parser->invalid || parser->depth >= 24) {
        parser->invalid = true;
        return 0;
    }
    bool negate = take(parser, "not");
    unsigned result = 0;
    if (take(parser, "(")) {
        parser->depth++;
        result = parse_filter_or(parser, attribute);
        parser->depth--;
        if (!take(parser, ")")) parser->invalid = true;
        if (negate) result ^= attribute ? ALL_ITEMS : ALL_EVENTS;
    } else if (negate) {
        parser->invalid = true;
    } else if (attribute) {
        result = parse_filter_comparison(parser);
    } else if (take(parser, "boolean") && take(parser, "(")) {
        parser->depth++;
        result = parse_item_path(parser);
        parser->depth--;
    } else {
        parser->invalid = true;
    }
    return result;
}

static unsigned parse_filter_and(item_parser_t *parser, bool attribute) {
    unsigned result = parse_filter_atom(parser, attribute);
    while (!parser->invalid && take(parser, "and"))
        result &= parse_filter_atom(parser, attribute);
    return result;
}

static unsigned parse_filter_or(item_parser_t *parser, bool attribute) {
    unsigned result = parse_filter_and(parser, attribute);
    while (!parser->invalid && take(parser, "or"))
        result |= parse_filter_and(parser, attribute);
    return result;
}

static bool parse_item_filter(char *expression, unsigned *mask) {
    /* The tag reader returns XML text with entities still encoded. */
    static const struct {const char *text; char value;} entities[] = {
        {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
    };
    char *out = expression;
    for (const char *in = expression; *in;) {
        if (*in != '&') {
            *out++ = *in++;
            continue;
        }
        unsigned i = 0;
        for (; i < sizeof(entities) / sizeof(entities[0]); i++) {
            size_t len = strlen(entities[i].text);
            if (strncmp(in, entities[i].text, len) != 0) continue;
            *out++ = entities[i].value;
            in += len;
            break;
        }
        if (i == sizeof(entities) / sizeof(entities[0])) return false;
    }
    *out = '\0';
    item_parser_t parser = {.p = expression};
    *mask = parse_filter_or(&parser, false);
    while (isspace((unsigned char)*parser.p)) parser.p++;
    return !parser.invalid && !*parser.p;
}

static void get_event_properties(int fd, const char *id) {
    static const char properties[] =
        "<tev:GetEventPropertiesResponse>"
        "<tev:TopicNamespaceLocation>http://www.onvif.org/ver10/topics/topicns.xml</tev:TopicNamespaceLocation>"
        "<wsnt:FixedTopicSet>true</wsnt:FixedTopicSet>"
        "<wstop:TopicSet><tns1:RuleEngine><CellMotionDetector><Motion wstop:topic=\"true\">"
        "<tt:MessageDescription IsProperty=\"true\"><tt:Source>"
        "<tt:SimpleItemDescription Name=\"VideoSourceConfigurationToken\" Type=\"tt:ReferenceToken\"/>"
        "<tt:SimpleItemDescription Name=\"VideoAnalyticsConfigurationToken\" Type=\"tt:ReferenceToken\"/>"
        "<tt:SimpleItemDescription Name=\"Rule\" Type=\"xs:string\"/>"
        "</tt:Source><tt:Data><tt:SimpleItemDescription Name=\"IsMotion\" Type=\"xs:boolean\"/>"
        "</tt:Data></tt:MessageDescription></Motion></CellMotionDetector></tns1:RuleEngine>"
        "<tns1:VideoSource><MotionAlarm wstop:topic=\"true\">"
        "<tt:MessageDescription IsProperty=\"true\"><tt:Source>"
        "<tt:SimpleItemDescription Name=\"Source\" Type=\"tt:ReferenceToken\"/>"
        "</tt:Source><tt:Data><tt:SimpleItemDescription Name=\"State\" Type=\"xs:boolean\"/>"
        "</tt:Data></tt:MessageDescription></MotionAlarm></tns1:VideoSource></wstop:TopicSet>"
        "<wsnt:TopicExpressionDialect>" CONCRETE_SET "</wsnt:TopicExpressionDialect>"
        "<wsnt:TopicExpressionDialect>" CONCRETE "</wsnt:TopicExpressionDialect>"
        "<tev:MessageContentFilterDialect>" ITEM_FILTER "</tev:MessageContentFilterDialect>"
        "<tev:MessageContentSchemaLocation>http://www.onvif.org/ver10/schema/onvif.xsd</tev:MessageContentSchemaLocation>"
        "</tev:GetEventPropertiesResponse>";
    event_reply(fd, EVENT_ACTION "GetEventPropertiesResponse", id, properties, false);
}

static void create_subscription(int fd, const char *xml, const char *id, const char *base_url,
                                uint32_t now) {
    int lease = subscription_lease(xml, "InitialTerminationTime");
    if (lease <= 0) {
        event_fault(fd, id, "wsnt:UnacceptableInitialTerminationTimeFault", "Invalid termination time");
        return;
    }
    char filter[1024];
    unsigned topics = onvif_xml_get_tag(xml, "TopicExpression", filter, sizeof(filter)) ?
                      filter_topics(filter) : ALL_EVENTS;
    unsigned items = ALL_EVENTS;
    if (onvif_xml_get_tag(xml, "MessageContent", filter, sizeof(filter)) &&
        (strlen(filter) == sizeof(filter) - 1 || !parse_item_filter(filter, &items))) {
        event_fault(fd, id, "wsnt:InvalidFilterFault", "Invalid ONVIF ItemFilter");
        return;
    }
    read_motion();

    subscription_t *s = NULL;
    for (unsigned i = 0; i < MAX_PULL_POINTS; i++) {
        if (!subscriptions[i].id[0]) {
            s = &subscriptions[i];
            break;
        }
    }
    if (!s) {
        event_fault(fd, id, "wsnt:SubscribeCreationFailedFault", "All pull points are in use");
        return;
    }
    memset(s, 0, sizeof(*s));
    if (!new_id(s->id)) {
        event_fault(fd, id, "wsnt:SubscribeCreationFailedFault", "Cannot allocate pull point");
        return;
    }
    s->pull_fd = -1;
    s->event_filter = topics & items;
    s->expires = now + (uint32_t)lease;
    enqueue(s, (notification_t){.utc = time(NULL), .active = motion_active, .initialized = true});

    char current[32], end[32], body[768];
    lease_times(s, now, current, end);
    snprintf(body, sizeof(body),
        "<tev:CreatePullPointSubscriptionResponse>"
        "<tev:SubscriptionReference><wsa:Address>%s" EVENTS_PATH "/%s</wsa:Address></tev:SubscriptionReference>"
        "<wsnt:CurrentTime>%s</wsnt:CurrentTime><wsnt:TerminationTime>%s</wsnt:TerminationTime>"
        "</tev:CreatePullPointSubscriptionResponse>", base_url, s->id, current, end);
    if (!event_reply(fd, EVENT_ACTION "CreatePullPointSubscriptionResponse", id, body, false))
        s->id[0] = '\0';
}

events_result_t events_request(int fd, const char *path, const char *action,
                              const char *xml, const char *base_url) {
    size_t path_len = sizeof(EVENTS_PATH) - 1;
    if (strncmp(path, EVENTS_PATH, path_len) != 0 || (path[path_len] && path[path_len] != '/'))
        return EVENTS_UNHANDLED;

    char id[192] = "";
    if (onvif_xml_get_tag(xml, "MessageID", id, sizeof(id)) && strpbrk(id, "<>&\"'"))
        id[0] = '\0';
    uint32_t now = monotonic_ms();
    if (!path[path_len] || !path[path_len + 1]) {
        if (strcmp(action, "GetEventProperties") == 0) {
            get_event_properties(fd, id);
        } else if (strcmp(action, "GetServiceCapabilities") == 0) {
            event_reply(fd, EVENT_ACTION "GetServiceCapabilitiesResponse", id,
                "<tev:GetServiceCapabilitiesResponse>" EVENT_CAPABILITIES "</tev:GetServiceCapabilitiesResponse>",
                false);
        } else if (strcmp(action, "CreatePullPointSubscription") == 0) {
            create_subscription(fd, xml, id, base_url, now);
        } else {
            event_fault(fd, id, "ter:ActionNotSupported", "Unsupported event operation");
        }
        return EVENTS_REPLIED;
    }

    subscription_t *s = NULL;
    for (unsigned i = 0; i < MAX_PULL_POINTS; i++) {
        subscription_t *candidate = &subscriptions[i];
        if (candidate->id[0] && strcmp(path + path_len + 1, candidate->id) == 0 &&
            !reached(now, candidate->expires)) {
            s = candidate;
            break;
        }
    }
    if (!s) {
        event_fault(fd, id, "wsrf-r:ResourceUnknownFault", "Unknown or expired subscription");
    } else if (strcmp(action, "PullMessages") == 0) {
        char value[48];
        int timeout = onvif_xml_get_tag(xml, "Timeout", value, sizeof(value)) ?
                      duration_ms(value, MAX_PULL_SEC * 1000) : -1;
        unsigned limit = EVENT_QUEUE_SIZE;
        if (onvif_xml_get_tag(xml, "MessageLimit", value, sizeof(value))) {
            char *end;
            unsigned long requested = strtoul(value, &end, 10);
            limit = !isdigit((unsigned char)value[0]) || *end || !requested ? 0 :
                    requested < limit ? (unsigned)requested : limit;
        }
        if (timeout < 0 || !limit || fd >= FD_SETSIZE) {
            event_fault(fd, id, "ter:InvalidArgVal", "Invalid pull timeout or limit");
        } else if (s->pull_fd >= 0) {
            event_fault(fd, id, "ter:OperationProhibited", "PullMessages already pending");
        } else {
            const struct timeval send_timeout = {0, 250000};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
            s->pull_fd = fd;
            s->pull_limit = limit;
            s->pull_deadline = now + (uint32_t)timeout;
            snprintf(s->relates_to, sizeof(s->relates_to), "%s", id);
            return EVENTS_PENDING;
        }
    } else if (strcmp(action, "SetSynchronizationPoint") == 0) {
        read_motion();
        enqueue(s, (notification_t){.utc = time(NULL), .active = motion_active, .initialized = true});
        event_reply(fd, PULL_ACTION "SetSynchronizationPointResponse", id,
                    "<tev:SetSynchronizationPointResponse/>", false);
    } else if (strcmp(action, "Renew") == 0) {
        int lease = subscription_lease(xml, "TerminationTime");
        if (lease <= 0) {
            event_fault(fd, id, "wsnt:UnacceptableTerminationTimeFault", "Invalid termination time");
        } else {
            s->expires = now + (uint32_t)lease;
            char current[32], end[32], body[256];
            lease_times(s, now, current, end);
            snprintf(body, sizeof(body),
                "<wsnt:RenewResponse><wsnt:TerminationTime>%s</wsnt:TerminationTime>"
                "<wsnt:CurrentTime>%s</wsnt:CurrentTime></wsnt:RenewResponse>", end, current);
            event_reply(fd, WSN_ACTION "RenewResponse", id, body, false);
        }
    } else if (strcmp(action, "Unsubscribe") == 0) {
        expire_subscription(s);
        event_reply(fd, WSN_ACTION "UnsubscribeResponse", id, "<wsnt:UnsubscribeResponse/>", false);
    } else {
        event_fault(fd, id, "ter:ActionNotSupported", "Unsupported subscription operation");
    }
    return EVENTS_REPLIED;
}
