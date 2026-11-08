/* Host-side radio capture; does not change the ESP firmware or Wi-Fi security.
 * Build: clang -Wall -Wextra -Werror -framework Foundation -framework CoreWLAN
 *        tools/macos_wifi_capture.m -lpcap -o dist/macos-wifi-capture
 */
#import <Foundation/Foundation.h>
#import <CoreWLAN/CoreWLAN.h>
#include <pcap/pcap.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *filter =
    "wlan host 00:23:4e:17:88:02 or "
    "(type mgt subtype beacon and wlan addr2 d0:cf:13:30:15:4d) or "
    "(type ctl and (wlan addr1 d0:cf:13:30:15:4d or wlan addr1 00:23:4e:17:88:02))";
static volatile sig_atomic_t stopped;
static void stop_capture(int sig) { (void)sig; stopped = 1; }
static double monotonic(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int check_filter(void)
{
    pcap_t *p = pcap_open_dead(DLT_IEEE802_11_RADIO, 4096);
    if (!p) return 1;
    struct bpf_program code;
    int rc = pcap_compile(p, &code, filter, 1, PCAP_NETMASK_UNKNOWN);
    if (rc) fprintf(stderr, "%s\n", pcap_geterr(p));
    else { pcap_freecode(&code); puts("PASS: radiotap capture filter compiles"); }
    pcap_close(p);
    return rc ? 1 : 0;
}

int main(int argc, const char *argv[])
{
    if (argc == 2 && !strcmp(argv[1], "--check")) return check_filter();
    if (argc != 2 || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "Usage: sudo %s OUTPUT.pcap\n"
            "Captures only PSP/AP radio frames on en0, channel 6, for 180 seconds.\n"
            "Wi-Fi disconnects temporarily; wired Internet/USB sharing must remain active.\n"
            "After capture, Wi-Fi power is restored; you may need to reconnect the saved network.\n"
            "The output must not exist. --check validates the filter without changing Wi-Fi.\n", argv[0]);
        return argc == 2 ? 0 : 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "Root required for /dev/bpf and monitor-mode channel selection. No settings changed.\n");
        return 1;
    }
    @autoreleasepool {
        CWInterface *iface = [[CWWiFiClient sharedWiFiClient] interfaceWithName:@"en0"];
        if (!iface || !iface.powerOn) {
            fprintf(stderr, "en0 Wi-Fi is unavailable or off. No settings changed.\n");
            return 1;
        }
        CWChannel *original = iface.wlanChannel, *target = nil;
        for (CWChannel *channel in iface.supportedWLANChannels) {
            if (channel.channelNumber == 6 && channel.channelBand == kCWChannelBand2GHz &&
                channel.channelWidth == kCWChannelWidth20MHz) { target = channel; break; }
        }
        if (!target) { fprintf(stderr, "2.4 GHz channel 6 is unavailable.\n"); return 1; }
        int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (fd < 0) { perror("create capture"); return 1; }
        const char *sudo_uid = getenv("SUDO_UID");
        if (sudo_uid && sudo_uid[0]) {
            char *end = NULL;
            unsigned long owner = strtoul(sudo_uid, &end, 10);
            if (*end || (uid_t)owner != owner || fchown(fd, (uid_t)owner, (gid_t)-1)) {
                fprintf(stderr, "Cannot assign capture to the invoking user.\n");
                close(fd); unlink(argv[1]); return 1;
            }
        }
        FILE *file = fdopen(fd, "wb");
        if (!file) { perror("fdopen"); close(fd); unlink(argv[1]); return 1; }
        char error[PCAP_ERRBUF_SIZE] = {0};
        pcap_t *capture = pcap_create("en0", error);
        if (!capture) { fprintf(stderr, "%s\n", error); fclose(file); unlink(argv[1]); return 1; }
        pcap_dumper_t *dump = NULL;
        BOOL changed = NO;
        int result = 1;
        unsigned packets = 0;
        signal(SIGINT, stop_capture);
        signal(SIGTERM, stop_capture);
        @try {
            if (pcap_set_snaplen(capture, 4096) || pcap_set_timeout(capture, 250) ||
                pcap_set_promisc(capture, 1) || pcap_set_rfmon(capture, 1)) {
                fprintf(stderr, "Monitor-mode setup: %s\n", pcap_geterr(capture));
            } else {
                [iface disassociate]; changed = YES;
                int activation = pcap_activate(capture);
                NSError *channelError = nil;
                if (activation < 0) {
                    fprintf(stderr, "Monitor mode: %s\n", pcap_geterr(capture));
                } else if (![iface setWLANChannel:target error:&channelError]) {
                    fprintf(stderr, "Channel 6: %s\n", channelError.localizedDescription.UTF8String);
                } else if (iface.wlanChannel.channelNumber != 6) {
                    fprintf(stderr, "Channel readback is not 6.\n");
                } else if (pcap_datalink(capture) != DLT_IEEE802_11_RADIO) {
                    fprintf(stderr, "Expected radiotap, received link type %d.\n", pcap_datalink(capture));
                } else {
                    struct bpf_program code;
                    if (pcap_compile(capture, &code, filter, 1, PCAP_NETMASK_UNKNOWN)) {
                        fprintf(stderr, "Filter: %s\n", pcap_geterr(capture));
                    } else {
                        int applied = pcap_setfilter(capture, &code);
                        pcap_freecode(&code);
                        if (applied || pcap_setnonblock(capture, 1, error)) {
                            fprintf(stderr, "Capture setup: %s %s\n", pcap_geterr(capture), error);
                        } else if (!(dump = pcap_dump_fopen(capture, file))) {
                            fprintf(stderr, "Capture output: %s\n", pcap_geterr(capture));
                        } else {
                            file = NULL; /* pcap dumper owns it. */
                            puts("READY: channel 6. Open http://192.168.50.1:8080/ without proxy; leave PSP connected for this comparison.");
                            fflush(stdout);
                            result = 0;
                            double deadline = monotonic() + 180;
                            while (!stopped && monotonic() < deadline) {
                                struct pcap_pkthdr *header;
                                const u_char *data;
                                int rc = pcap_next_ex(capture, &header, &data);
                                if (rc == 1) {
                                    pcap_dump((u_char *)dump, header, data); ++packets;
                                    if (!(packets % 32)) pcap_dump_flush(dump);
                                } else if (rc < 0) {
                                    fprintf(stderr, "Capture read: %s\n", pcap_geterr(capture));
                                    result = 1; break;
                                } else usleep(20000);
                            }
                            if (pcap_dump_flush(dump)) { perror("flush capture"); result = 1; }
                        }
                    }
                }
            }
        } @finally {
            if (dump) pcap_dump_close(dump);
            if (file) fclose(file);
            pcap_close(capture);
            if (changed) {
                NSError *restoreError = nil;
                if (original) [iface setWLANChannel:original error:&restoreError];
                BOOL off = [iface setPower:NO error:&restoreError];
                if (off) usleep(200000);
                BOOL on = [iface setPower:YES error:&restoreError];
                if (!off || !on) {
                    fprintf(stderr, "Wi-Fi restore: %s; re-enable Wi-Fi in macOS.\n",
                            restoreError.localizedDescription.UTF8String);
                    result = 1;
                }
            }
        }
        printf("DONE: %u filtered packets saved to %s. Wi-Fi power restored; reconnect its saved network if needed.\n", packets, argv[1]);
        return result;
    }
}
