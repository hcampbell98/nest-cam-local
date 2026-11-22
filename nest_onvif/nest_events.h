#ifndef NEST_EVENTS_H
#define NEST_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/select.h>

#define EVENTS_PATH "/onvif/events_service"
#define EVENTS_NS "http://www.onvif.org/ver10/events/wsdl"
#define EVENT_CAPABILITIES \
    "<tev:Capabilities WSSubscriptionPolicySupport=\"false\"" \
    " WSPausableSubscriptionManagerInterfaceSupport=\"false\" WSPullPointSupport=\"true\"" \
    " MaxPullPoints=\"8\" PersistentNotificationStorage=\"false\"/>"

#define MOTION_VIDEO_TOKEN "VideoSource_1"
#define MOTION_SOURCE_TOKEN "VideoSourceConfig_1"
#define MOTION_ANALYTICS_TOKEN "VideoAnalyticsConfig_1"
#define MOTION_RULE "MotionDetector"

typedef enum { EVENTS_UNHANDLED, EVENTS_REPLIED, EVENTS_PENDING } events_result_t;

bool onvif_xml_get_tag(const char *xml, const char *local_name, char *out, size_t size);
bool onvif_send_all(int fd, const void *buf, size_t len);

events_result_t events_request(int fd, const char *path, const char *action,
                              const char *xml, const char *base_url);
void events_poll(fd_set *fds, int *max_fd, struct timeval *timeout);
void events_readable(const fd_set *fds);
void events_close(void);

#endif
