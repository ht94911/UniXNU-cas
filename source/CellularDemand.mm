// SPDX-License-Identifier: BSD-2-Clause
// One ordinary Network.framework request on the existing cellular interface.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <Network/Network.h>
#include <dispatch/dispatch.h>
#include <os/log.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "QcomUserLogTransport.h"

static dispatch_queue_t queue;
static nw_connection_t connection;
static bool finished, sent, cellular, pdp;
static size_t sent_bytes, received_bytes;
static char header[8193];
static size_t header_bytes;
static int http_status;

static void report(const char *event, int value)
{
    char text[400];
    snprintf(text, sizeof text,
        "BB21 event=%s value=%d cellular=%u pdp=%u sent=%zu received=%zu http=%d",
        event, value, cellular, pdp, sent_bytes, received_bytes, http_status);
    os_log(OS_LOG_DEFAULT, "%{public}s", text);
    (void)QcomUserLog::status(text);
}

static void timeout(int signal_number)
{
    (void)signal_number;
    static const char text[] = "BB21 own helper timeout\n";
    (void)write(STDERR_FILENO, text, sizeof text - 1);
    _exit(124);
}

static bool selected_payload(void)
{
    io_registry_entry_t chosen = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/chosen");
    if (!chosen) return false;
    CFTypeRef value = IORegistryEntryCreateCFProperty(chosen, CFSTR("nezha-cellular-demand"),
                                                     kCFAllocatorDefault, 0);
    IOObjectRelease(chosen);
    bool selected = false;
    if (value && CFGetTypeID(value) == CFDataGetTypeID()
        && CFDataGetLength((CFDataRef)value) == 4) {
        const UInt8 *bytes = CFDataGetBytePtr((CFDataRef)value);
        selected = bytes[0] == 21 && !bytes[1] && !bytes[2] && !bytes[3];
    }
    if (value) CFRelease(value);
    return selected;
}

static void finish(int code)
{
    if (finished) return;
    finished = true;
    report(code ? "request_failed" : "HTTPS_cellular_complete", code);
    if (connection) nw_connection_cancel(connection);
    alarm(0);
    exit(code);
}

static void check_path(void)
{
    nw_path_t path = nw_connection_copy_current_path(connection);
    cellular = path && nw_path_uses_interface_type(path, nw_interface_type_cellular);
    pdp = false;
    if (path) nw_path_enumerate_interfaces(path, ^bool(nw_interface_t interface) {
        const char *name = nw_interface_get_name(interface);
        if (name && !strncmp(name, "pdp_ip", 6)
            && nw_interface_get_type(interface) == nw_interface_type_cellular) pdp = true;
        return true;
    });
}

static void receive_response(void)
{
    if (finished) return;
    nw_connection_receive(connection, 1, 4096,
        ^(dispatch_data_t content, nw_content_context_t context, bool complete, nw_error_t error) {
            (void)context;
            if (finished) return;
            if (content) {
                size_t bytes = dispatch_data_get_size(content);
                if (bytes > 65536 - received_bytes) { finish(8); return; }
                received_bytes += bytes;
                (void)dispatch_data_apply(content,
                    ^bool(dispatch_data_t region, size_t offset, const void *data, size_t size) {
                        (void)region; (void)offset;
                        size_t take = size;
                        if (take > sizeof header - 1 - header_bytes)
                            take = sizeof header - 1 - header_bytes;
                        memcpy(header + header_bytes, data, take);
                        header_bytes += take;
                        header[header_bytes] = 0;
                        return true;
                    });
            }
            if (strstr(header, "\r\n\r\n")) {
                unsigned status = 0;
                if (sscanf(header, "HTTP/1.%*u %u", &status) != 1) { finish(9); return; }
                http_status = (int)status;
                check_path();
                report("response_header", http_status);
                finish(cellular && pdp && sent_bytes && received_bytes && http_status == 204 ? 0 : 10);
                return;
            }
            if (error) { report("receive_error", nw_error_get_error_code(error)); finish(6); return; }
            if (complete || header_bytes == sizeof header - 1) { finish(7); return; }
            receive_response();
        });
}

static void start_request(void)
{
    nw_parameters_t parameters = nw_parameters_create_secure_tcp(
        NW_PARAMETERS_DEFAULT_CONFIGURATION, NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_parameters_set_required_interface_type(parameters, nw_interface_type_cellular);
    nw_parameters_set_prohibit_expensive(parameters, false);
    nw_endpoint_t endpoint = nw_endpoint_create_host("www.gstatic.com", "443");
    connection = nw_connection_create(endpoint, parameters);
    if (!connection) { finish(2); return; }
    nw_connection_set_queue(connection, queue);
    nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t state, nw_error_t error) {
        if (finished) return;
        report("connection_state", (int)state);
        if (error) report("connection_error", nw_error_get_error_code(error));
        if (state == nw_connection_state_failed) { finish(3); return; }
        if (state != nw_connection_state_ready || sent) return;
        check_path();
        if (!cellular || !pdp) { finish(4); return; }
        sent = true;
        static const char request[] =
            "GET /generate_204 HTTP/1.1\r\nHost: www.gstatic.com\r\n"
            "Connection: close\r\nUser-Agent: UniXNU-Cellular-Test\r\n\r\n";
        dispatch_data_t data = dispatch_data_create(request, sizeof request - 1, queue,
                                                     DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        nw_connection_send(connection, data, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, false,
            ^(nw_error_t send_error) {
                if (finished) return;
                if (send_error) {
                    report("send_error", nw_error_get_error_code(send_error)); finish(5); return;
                }
                sent_bytes = sizeof request - 1;
                report("request_sent", 0);
                receive_response();
            });
    });
    report("one_cellular_request_started", 0);
    nw_connection_start(connection);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 180 * NSEC_PER_SEC), queue, ^{
        if (!finished) finish(11);
    });
}

int main(void)
{
    if (geteuid() != 0 || !selected_payload()) return 0;
    signal(SIGALRM, timeout);
    alarm(400);
    queue = dispatch_queue_create("org.unixnu.nezha.cellular-demand", DISPATCH_QUEUE_SERIAL);
    struct timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 1;
    int64_t delay = now.tv_sec < 150 ? 150 - now.tv_sec : 0;
    report("selected_payload_waiting_for_radio", (int)delay);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay * NSEC_PER_SEC), queue, ^{
        start_request();
    });
    dispatch_main();
}
