/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bootspec.h"
#include "chase.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "discover-image.h"
#include "sd-daemon.h"
#include "sd-event.h"
#include "sd-id128.h"

#include "alloc-util.h"
#include "architecture.h"
#include "build.h"
#include "common-signal.h"
#include "copy.h"
#include "creds-util.h"
#include "dissect-image.h"
#include "escape.h"
#include "event-util.h"
#include "extract-word.h"
#include "fileio.h"
#include "format-util.h"
#include "fs-util.h"
#include "gpt.h"
#include "hexdecoct.h"
#include "hostname-util.h"
#include "kernel-image.h"
#include "log.h"
#include "machine-credential.h"
#include "macro.h"
#include "main-func.h"
#include "mkdir.h"
#include "pager.h"
#include "parse-argument.h"
#include "parse-util.h"
#include "path-lookup.h"
#include "path-util.h"
#include "pretty-print.h"
#include "process-util.h"
#include "random-util.h"
#include "rm-rf.h"
#include "signal-util.h"
#include "socket-util.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "tmpfile-util.h"
#include "unit-name.h"
#include "vmspawn-mount.h"
#include "vmspawn-scope.h"
#include "vmspawn-settings.h"
#include "vmspawn-util.h"

static bool arg_quiet = false;
static PagerFlags arg_pager_flags = 0;
static char *arg_directory = NULL;
static char *arg_image = NULL;
static char *arg_machine = NULL;
static char *arg_qemu_smp = NULL;
static uint64_t arg_qemu_mem = UINT64_C(2) * U64_GB;
static int arg_qemu_kvm = -1;
static int arg_qemu_vsock = -1;
static unsigned arg_vsock_cid = VMADDR_CID_ANY;
static int arg_tpm = -1;
static char *arg_linux = NULL;
static char **arg_initrds = NULL;
static bool arg_qemu_gui = false;
static QemuNetworkStack arg_network_stack = QEMU_NET_NONE;
static int arg_secure_boot = -1;
static MachineCredentialContext arg_credentials = {};
static uid_t arg_uid_shift = UID_INVALID, arg_uid_range = 0x10000U;
static RuntimeMountContext arg_runtime_mounts = {};
static SettingsMask arg_settings_mask = 0;
static char *arg_firmware = NULL;
static char *arg_runtime_directory = NULL;
static char *arg_forward_journal = NULL;
static bool arg_runtime_directory_created = false;
static bool arg_privileged = false;
static char **arg_kernel_cmdline_extra = NULL;

STATIC_DESTRUCTOR_REGISTER(arg_directory, freep);
STATIC_DESTRUCTOR_REGISTER(arg_image, freep);
STATIC_DESTRUCTOR_REGISTER(arg_machine, freep);
STATIC_DESTRUCTOR_REGISTER(arg_qemu_smp, freep);
STATIC_DESTRUCTOR_REGISTER(arg_runtime_directory, freep);
STATIC_DESTRUCTOR_REGISTER(arg_credentials, machine_credential_context_done);
STATIC_DESTRUCTOR_REGISTER(arg_firmware, freep);
STATIC_DESTRUCTOR_REGISTER(arg_linux, freep);
STATIC_DESTRUCTOR_REGISTER(arg_initrds, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_runtime_mounts, runtime_mount_context_done);
STATIC_DESTRUCTOR_REGISTER(arg_forward_journal, freep);
STATIC_DESTRUCTOR_REGISTER(arg_kernel_cmdline_extra, strv_freep);

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        pager_open(arg_pager_flags);

        r = terminal_urlify_man("systemd-vmspawn", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s [OPTIONS...] [ARGUMENTS...]\n\n"
               "%5$sSpawn a command or OS in a virtual machine.%6$s\n\n"
               "  -h --help                Show this help\n"
               "     --version             Print version string\n"
               "  -q --quiet               Do not show status information\n"
               "     --no-pager            Do not pipe output into a pager\n"
               "\n%3$sImage:%4$s\n"
               "  -D --directory=PATH      Root directory for the VM\n"
               "  -i --image=FILE|DEVICE   Root file system disk image or device for the VM\n"
               "\n%3$sHost Configuration:%4$s\n"
               "     --qemu-smp=SMP        Configure guest's SMP settings\n"
               "     --qemu-mem=MEM        Configure guest's RAM size\n"
               "     --qemu-kvm=BOOL       Enable use of KVM\n"
               "     --qemu-vsock=BOOL     Override autodetection of VSock support in QEMU\n"
               "     --vsock-cid=CID       Specify the CID to use for the qemu guest's VSock\n"
               "     --tpm=BOOL            Enable use of a virtual TPM\n"
               "     --linux=PATH          Specify the linux kernel for direct kernel boot\n"
               "     --initrd=PATH         Specify the initrd for direct kernel boot\n"
               "     --qemu-gui            Start QEMU in graphical mode\n"
               "  -n --network-tap         Create a TAP device for networking with QEMU\n"
               "     --network-user-mode   Use user mode networking with QEMU\n"
               "     --secure-boot=BOOL    Enable searching for firmware supporting SecureBoot\n"
               "     --firmware=PATH|list  Select firmware definition file (or list available)\n"
               "\n%3$sSystem Identity:%4$s\n"
               "  -M --machine=NAME        Set the machine name for the VM\n"
               "\n%3$sUser Namespacing:%4$s\n"
               "     --private-users=UIDBASE[:NUIDS]\n"
               "                           Configure the UID/GID range to map into the\n"
               "                           virtiofsd namespace\n"
               "\n%3$sMounts:%4$s\n"
               "     --bind=SOURCE[:TARGET]\n"
               "                           Mount a file or directory from the host into the VM\n"
               "     --bind-ro=SOURCE[:TARGET]\n"
               "                           Mount a file or directory, but read-only\n"
               "\n%3$sIntegration:%4$s\n"
               "     --forward-journal=FILE|DIR\n"
               "                           Forward the VM's journal to the host\n"
               "\n%3$sCredentials:%4$s\n"
               "     --set-credential=ID:VALUE\n"
               "                           Pass a credential with literal value to the VM\n"
               "     --load-credential=ID:PATH\n"
               "                           Load credential for the VM from file or AF_UNIX\n"
               "                           stream socket.\n"
               "\nSee the %2$s for details.\n",
               program_invocation_short_name,
               link,
               ansi_underline(),
               ansi_normal(),
               ansi_highlight(),
               ansi_normal());

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_NO_PAGER,
                ARG_QEMU_SMP,
                ARG_QEMU_MEM,
                ARG_QEMU_KVM,
                ARG_QEMU_VSOCK,
                ARG_VSOCK_CID,
                ARG_TPM,
                ARG_LINUX,
                ARG_INITRD,
                ARG_QEMU_GUI,
                ARG_NETWORK_USER_MODE,
                ARG_BIND,
                ARG_BIND_RO,
                ARG_SECURE_BOOT,
                ARG_PRIVATE_USERS,
                ARG_FORWARD_JOURNAL,
                ARG_SET_CREDENTIAL,
                ARG_LOAD_CREDENTIAL,
                ARG_FIRMWARE,
        };

        static const struct option options[] = {
                { "help",              no_argument,       NULL, 'h'                   },
                { "version",           no_argument,       NULL, ARG_VERSION           },
                { "quiet",             no_argument,       NULL, 'q'                   },
                { "no-pager",          no_argument,       NULL, ARG_NO_PAGER          },
                { "image",             required_argument, NULL, 'i'                   },
                { "directory",         required_argument, NULL, 'D'                   },
                { "machine",           required_argument, NULL, 'M'                   },
                { "qemu-smp",          required_argument, NULL, ARG_QEMU_SMP          },
                { "qemu-mem",          required_argument, NULL, ARG_QEMU_MEM          },
                { "qemu-kvm",          required_argument, NULL, ARG_QEMU_KVM          },
                { "qemu-vsock",        required_argument, NULL, ARG_QEMU_VSOCK        },
                { "vsock-cid",         required_argument, NULL, ARG_VSOCK_CID         },
                { "tpm",               required_argument, NULL, ARG_TPM               },
                { "linux",             required_argument, NULL, ARG_LINUX             },
                { "initrd",            required_argument, NULL, ARG_INITRD            },
                { "qemu-gui",          no_argument,       NULL, ARG_QEMU_GUI          },
                { "network-tap",       no_argument,       NULL, 'n'                   },
                { "network-user-mode", no_argument,       NULL, ARG_NETWORK_USER_MODE },
                { "bind",              required_argument, NULL, ARG_BIND              },
                { "bind-ro",           required_argument, NULL, ARG_BIND_RO           },
                { "secure-boot",       required_argument, NULL, ARG_SECURE_BOOT       },
                { "private-users",     required_argument, NULL, ARG_PRIVATE_USERS     },
                { "forward-journal",   required_argument, NULL, ARG_FORWARD_JOURNAL   },
                { "set-credential",    required_argument, NULL, ARG_SET_CREDENTIAL    },
                { "load-credential",   required_argument, NULL, ARG_LOAD_CREDENTIAL   },
                { "firmware",          required_argument, NULL, ARG_FIRMWARE          },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        optind = 0;
        while ((c = getopt_long(argc, argv, "+hD:i:M:nq", options, NULL)) >= 0)
                switch (c) {
                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case 'q':
                        arg_quiet = true;
                        break;

                case 'D':
                        r = parse_path_argument(optarg, /* suppress_root= */ false, &arg_directory);
                        if (r < 0)
                                return r;

                        arg_settings_mask |= SETTING_DIRECTORY;
                        break;

                case 'i':
                        r = parse_path_argument(optarg, /* suppress_root= */ false, &arg_image);
                        if (r < 0)
                                return r;

                        arg_settings_mask |= SETTING_DIRECTORY;
                        break;

                case 'M':
                        if (isempty(optarg))
                                arg_machine = mfree(arg_machine);
                        else {
                                if (!hostname_is_valid(optarg, 0))
                                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                               "Invalid machine name: %s", optarg);

                                r = free_and_strdup(&arg_machine, optarg);
                                if (r < 0)
                                        return log_oom();
                        }
                        break;

                case ARG_NO_PAGER:
                        arg_pager_flags |= PAGER_DISABLE;
                        break;

                case ARG_QEMU_SMP:
                        r = free_and_strdup_warn(&arg_qemu_smp, optarg);
                        if (r < 0)
                                return r;
                        break;

                case ARG_QEMU_MEM:
                        r = parse_size(optarg, 1024, &arg_qemu_mem);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --qemu-mem=%s: %m", optarg);
                        break;

                case ARG_QEMU_KVM:
                        r = parse_tristate(optarg, &arg_qemu_kvm);
                        if (r < 0)
                            return log_error_errno(r, "Failed to parse --qemu-kvm=%s: %m", optarg);
                        break;

                case ARG_QEMU_VSOCK:
                        r = parse_tristate(optarg, &arg_qemu_vsock);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --qemu-vsock=%s: %m", optarg);
                        break;

                case ARG_VSOCK_CID:
                        if (isempty(optarg))
                                arg_vsock_cid = VMADDR_CID_ANY;
                        else {
                                unsigned cid;

                                r = vsock_parse_cid(optarg, &cid);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to parse --vsock-cid: %s", optarg);
                                if (!VSOCK_CID_IS_REGULAR(cid))
                                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Specified CID is not regular, refusing: %u", cid);

                                arg_vsock_cid = cid;
                        }
                        break;

                case ARG_TPM:
                        r = parse_tristate(optarg, &arg_tpm);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --tpm=%s: %m", optarg);
                        break;

                case ARG_LINUX:
                        r = parse_path_argument(optarg, /* suppress_root= */ false, &arg_linux);
                        if (r < 0)
                                return r;
                        break;

                case ARG_INITRD: {
                        _cleanup_free_ char *initrd_path = NULL;
                        r = parse_path_argument(optarg, /* suppress_root= */ false, &initrd_path);
                        if (r < 0)
                                return r;

                        r = strv_consume(&arg_initrds, TAKE_PTR(initrd_path));
                        if (r < 0)
                                return log_oom();

                        break;
                }

                case ARG_QEMU_GUI:
                        arg_qemu_gui = true;
                        break;

                case 'n':
                        arg_network_stack = QEMU_NET_TAP;
                        break;

                case ARG_NETWORK_USER_MODE:
                        arg_network_stack = QEMU_NET_USER;
                        break;

                case ARG_BIND:
                case ARG_BIND_RO:
                        r = runtime_mount_parse(&arg_runtime_mounts, optarg, c == ARG_BIND_RO);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --bind(-ro)= argument %s: %m", optarg);

                        arg_settings_mask |= SETTING_BIND_MOUNTS;
                        break;

                case ARG_SECURE_BOOT:
                        r = parse_tristate(optarg, &arg_secure_boot);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --secure-boot=%s: %m", optarg);
                        break;

                case ARG_PRIVATE_USERS:
                        r = parse_userns_uid_range(optarg, &arg_uid_shift, &arg_uid_range);
                        if (r < 0)
                                return r;
                        break;

                case ARG_FORWARD_JOURNAL:
                        r = parse_path_argument(optarg, /* suppress_root= */ false, &arg_forward_journal);
                        if (r < 0)
                                return r;
                        break;

                case ARG_SET_CREDENTIAL: {
                        r = machine_credential_set(&arg_credentials, optarg);
                        if (r < 0)
                                return r;
                        arg_settings_mask |= SETTING_CREDENTIALS;
                        break;
                }

                case ARG_LOAD_CREDENTIAL: {
                        r = machine_credential_load(&arg_credentials, optarg);
                        if (r < 0)
                                return r;

                        arg_settings_mask |= SETTING_CREDENTIALS;
                        break;
                }

                case ARG_FIRMWARE:
                        if (streq(optarg, "list")) {
                                _cleanup_strv_free_ char **l = NULL;

                                r = list_ovmf_config(&l);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to list firmwares: %m");

                                bool nl = false;
                                fputstrv(stdout, l, "\n", &nl);
                                if (nl)
                                        putchar('\n');

                                return 0;
                        }

                        if (!isempty(optarg) && !path_is_absolute(optarg) && !startswith(optarg, "./"))
                                return log_error_errno(SYNTHETIC_ERRNO(errno), "Absolute path or path starting with './' required.");

                        r = parse_path_argument(optarg, /* suppress_root= */ false, &arg_firmware);
                        if (r < 0)
                                return r;

                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (argc > optind) {
                arg_kernel_cmdline_extra = strv_copy(argv + optind);
                if (!arg_kernel_cmdline_extra)
                        return log_oom();

                arg_settings_mask |= SETTING_START_MODE;
        }

        return 1;
}

static int open_vsock(void) {
        _cleanup_close_ int vsock_fd = -EBADF;
        int r;
        static const union sockaddr_union bind_addr = {
                .vm.svm_family = AF_VSOCK,
                .vm.svm_cid = VMADDR_CID_ANY,
                .vm.svm_port = VMADDR_PORT_ANY,
        };

        vsock_fd = socket(AF_VSOCK, SOCK_STREAM|SOCK_CLOEXEC, 0);
        if (vsock_fd < 0)
                return log_error_errno(errno, "Failed to open AF_VSOCK socket: %m");

        r = bind(vsock_fd, &bind_addr.sa, sizeof(bind_addr.vm));
        if (r < 0)
                return log_error_errno(errno, "Failed to bind to vsock to address %u:%u: %m", bind_addr.vm.svm_cid, bind_addr.vm.svm_port);

        r = listen(vsock_fd, SOMAXCONN_DELUXE);
        if (r < 0)
                return log_error_errno(errno, "Failed to listen on vsock: %m");

        return TAKE_FD(vsock_fd);
}

static int vmspawn_dispatch_notify_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        char buf[NOTIFY_BUFFER_MAX+1];
        const char *p = NULL;
        struct iovec iovec = {
                .iov_base = buf,
                .iov_len = sizeof(buf)-1,
        };
        struct msghdr msghdr = {
                .msg_iov = &iovec,
                .msg_iovlen = 1,
        };
        ssize_t n;
        _cleanup_strv_free_ char **tags = NULL;
        int r, *exit_status = ASSERT_PTR(userdata);

        n = recvmsg_safe(fd, &msghdr, MSG_DONTWAIT);
        if (ERRNO_IS_NEG_TRANSIENT(n))
                return 0;
        if (n == -EXFULL) {
                log_warning_errno(n, "Got message with truncated control data, ignoring: %m");
                return 0;
        }
        if (n < 0)
                return log_warning_errno(n, "Couldn't read notification socket: %m");

        if ((size_t) n >= sizeof(buf)) {
                log_warning("Received notify message exceeded maximum size. Ignoring.");
                return 0;
        }

        buf[n] = 0;
        tags = strv_split(buf, "\n\r");
        if (!tags)
                return log_oom();

        STRV_FOREACH(s, tags)
                log_debug("Received tag %s from notify socket", *s);

        if (strv_contains(tags, "READY=1")) {
                r = sd_notify(false, "READY=1\n");
                if (r < 0)
                        log_warning_errno(r, "Failed to send readiness notification, ignoring: %m");
        }

        p = strv_find_startswith(tags, "STATUS=");
        if (p)
                (void) sd_notifyf(false, "STATUS=VM running: %s", p);

        p = strv_find_startswith(tags, "EXIT_STATUS=");
        if (p) {
                r = safe_atoi(p, exit_status);
                if (r < 0)
                        log_warning_errno(r, "Failed to parse exit status from %s, ignoring: %m", p);
        }

        /* we will only receive one message from each connection so disable this source once one is received */
        source = sd_event_source_disable_unref(source);

        return 0;
}

static int vmspawn_dispatch_vsock_connections(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        int r;
        sd_event *event;
        _cleanup_close_ int conn_fd = -EBADF;

        assert(userdata);

        if (revents != EPOLLIN) {
                log_warning("Got unexpected poll event for vsock fd.");
                return 0;
        }

        conn_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC|SOCK_NONBLOCK);
        if (conn_fd < 0) {
                log_warning_errno(errno, "Failed to accept connection from vsock fd (%m), ignoring...");
                return 0;
        }

        event = sd_event_source_get_event(source);
        if (!event)
                return log_error_errno(SYNTHETIC_ERRNO(ENOENT), "Failed to retrieve event from event source, exiting task");

        /* add a new floating task to read from the connection */
        r = sd_event_add_io(event, NULL, conn_fd, revents, vmspawn_dispatch_notify_fd, userdata);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate notify connection event source: %m");

        /* conn_fd is now owned by the event loop so don't clean it up */
        TAKE_FD(conn_fd);

        return 0;
}

static int setup_notify_parent(sd_event *event, int fd, int *exit_status, sd_event_source **ret_notify_event_source) {
        int r;

        assert(event);
        assert(fd >= 0);
        assert(exit_status);
        assert(ret_notify_event_source);

        r = sd_event_add_io(event, ret_notify_event_source, fd, EPOLLIN, vmspawn_dispatch_vsock_connections, exit_status);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate notify socket event source: %m");

        (void) sd_event_source_set_description(*ret_notify_event_source, "vmspawn-notify-sock");

        return 0;
}

static int on_orderly_shutdown(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        PidRef *pidref = userdata;
        int r;

        /* TODO: actually talk to qemu and ask the guest to shutdown here */

        if (pidref) {
                r = pidref_kill(pidref, SIGKILL);
                if (r < 0)
                        log_warning_errno(r, "Failed to kill qemu, terminating: %m");
                else {
                        log_info("Trying to halt qemu. Send SIGTERM again to trigger vmspawn to immediately terminate.");
                        sd_event_source_set_userdata(s, NULL);
                        return 0;
                }
        }

        sd_event_exit(sd_event_source_get_event(s), 0);
        return 0;
}

static int on_child_exit(sd_event_source *s, const siginfo_t *si, void *userdata) {
        sd_event_exit(sd_event_source_get_event(s), 0);
        return 0;
}

static int cmdline_add_vsock(char ***cmdline, int vsock_fd) {
        int r;

        r = strv_extend(cmdline, "-smbios");
        if (r < 0)
                return r;

        union sockaddr_union addr;
        socklen_t addr_len = sizeof addr.vm;
        r = getsockname(vsock_fd, &addr.sa, &addr_len);
        if (r < 0)
                return -errno;
        assert(addr_len >= sizeof addr.vm);
        assert(addr.vm.svm_family == AF_VSOCK);

        r = strv_extendf(cmdline, "type=11,value=io.systemd.credential:vmm.notify_socket=vsock-stream:%u:%u", (unsigned) VMADDR_CID_HOST, addr.vm.svm_port);
        if (r < 0)
                return r;

        return 0;
}

static int start_tpm(sd_bus *bus, const char *scope, const char *tpm, const char **ret_state_tempdir) {
        _cleanup_(rm_rf_physical_and_freep) char *state_dir = NULL;
        _cleanup_free_ char *scope_prefix = NULL;
        _cleanup_(socket_service_pair_done) SocketServicePair ssp = {
                .socket_type = SOCK_STREAM,
        };
        int r;

        assert(bus);
        assert(scope);
        assert(tpm);
        assert(ret_state_tempdir);

        r = unit_name_to_prefix(scope, &scope_prefix);
        if (r < 0)
                return log_error_errno(r, "Failed to strip .scope suffix from scope: %m");

        ssp.unit_name_prefix = strjoin(scope_prefix, "-tpm");
        if (!ssp.unit_name_prefix)
                return log_oom();

        state_dir = path_join(arg_runtime_directory, ssp.unit_name_prefix);
        if (!state_dir)
                return log_oom();

        if (arg_runtime_directory_created) {
                ssp.runtime_directory = path_join("systemd/vmspawn", ssp.unit_name_prefix);
                if (!ssp.runtime_directory)
                        return log_oom();
        }

        ssp.listen_address = path_join(state_dir, "sock");
        if (!ssp.listen_address)
                return log_oom();

        ssp.exec_start = strv_new(tpm, "socket", "--tpm2", "--tpmstate");
        if (!ssp.exec_start)
                return log_oom();

        r = strv_extendf(&ssp.exec_start, "dir=%s", state_dir);
        if (r < 0)
                return log_oom();

        r = strv_extend_many(&ssp.exec_start, "--ctrl", "type=unixio,fd=3");
        if (r < 0)
                return log_oom();

        r = start_socket_service_pair(bus, scope, &ssp);
        if (r < 0)
                return r;

        *ret_state_tempdir = TAKE_PTR(state_dir);

        return 0;
}

static int start_systemd_journal_remote(sd_bus *bus, const char *scope, unsigned port, const char *sd_journal_remote, char **listen_address) {
        _cleanup_free_ char *scope_prefix = NULL;
        _cleanup_(socket_service_pair_done) SocketServicePair ssp = {
                .socket_type = SOCK_STREAM,
        };
        int r;

        assert(bus);
        assert(scope);
        assert(sd_journal_remote);

        r = unit_name_to_prefix(scope, &scope_prefix);
        if (r < 0)
                return log_error_errno(r, "Failed to strip .scope suffix from scope: %m");

        ssp.unit_name_prefix = strjoin(scope_prefix, "-forward-journal");
        if (!ssp.unit_name_prefix)
                return log_oom();

        r = asprintf(&ssp.listen_address, "vsock:2:%u", port);
        if (r < 0)
                return log_oom();

        ssp.exec_start = strv_new(sd_journal_remote,
                        "--output", arg_forward_journal,
                        "--split-mode", endswith(arg_forward_journal, ".journal") ? "none" : "host");
        if (!ssp.exec_start)
                return log_oom();

        r = start_socket_service_pair(bus, scope, &ssp);
        if (r < 0)
                return r;

        if (listen_address)
                *listen_address = TAKE_PTR(ssp.listen_address);

        return 0;
}

static int discover_root(char **ret) {
        int r;
        _cleanup_(dissected_image_unrefp) DissectedImage *image = NULL;
        _cleanup_free_ char *root = NULL;

        assert(ret);

        r = dissect_image_file_and_warn(
                        arg_image,
                        /* verity= */ NULL,
                        /* mount_options= */ NULL,
                        /* image_policy= */ NULL,
                        /* flags= */ 0,
                        &image);
        if (r < 0)
                return r;

        if (image->partitions[PARTITION_ROOT].found)
                root = strjoin("root=PARTUUID=", SD_ID128_TO_UUID_STRING(image->partitions[PARTITION_ROOT].uuid));
        else if (image->partitions[PARTITION_USR].found)
                root = strjoin("mount.usr=PARTUUID=", SD_ID128_TO_UUID_STRING(image->partitions[PARTITION_USR].uuid));
        else
                return log_error_errno(SYNTHETIC_ERRNO(ENOENT), "Cannot perform a direct kernel boot without a root or usr partition, refusing");

        if (!root)
                return log_oom();

        *ret = TAKE_PTR(root);
        return 0;
}

static int find_virtiofsd(char **ret) {
        int r;
        _cleanup_free_ char *virtiofsd = NULL;

        assert(ret);

        r = find_executable("virtiofsd", &virtiofsd);
        if (r < 0 && r != -ENOENT)
                return log_error_errno(r, "Error while searching for virtiofsd: %m");

        if (!virtiofsd) {
                FOREACH_STRING(file, "/usr/libexec/virtiofsd", "/usr/lib/virtiofsd") {
                        if (access(file, X_OK) >= 0) {
                                virtiofsd = strdup(file);
                                if (!virtiofsd)
                                        return log_oom();
                                break;
                        }

                        if (!IN_SET(errno, ENOENT, EACCES))
                                return log_error_errno(errno, "Error while searching for virtiofsd: %m");
                }
        }

        if (!virtiofsd)
                return log_error_errno(SYNTHETIC_ERRNO(ENOENT), "Failed to find virtiofsd binary.");

        *ret = TAKE_PTR(virtiofsd);
        return 0;
}

static int start_virtiofsd(sd_bus *bus, const char *scope, const char *directory, bool uidmap, char **ret_state_tempdir, char **ret_sock_name) {
        _cleanup_(rm_rf_physical_and_freep) char *state_dir = NULL;
        _cleanup_free_ char *virtiofsd = NULL, *sock_name = NULL, *scope_prefix = NULL;
        _cleanup_(socket_service_pair_done) SocketServicePair ssp = {
                .socket_type = SOCK_STREAM,
        };
        static unsigned virtiofsd_instance = 0;
        int r;

        assert(bus);
        assert(scope);
        assert(directory);
        assert(ret_state_tempdir);
        assert(ret_sock_name);

        r = find_virtiofsd(&virtiofsd);
        if (r < 0)
                return r;

        r = unit_name_to_prefix(scope, &scope_prefix);
        if (r < 0)
                return log_error_errno(r, "Failed to strip .scope suffix from scope: %m");

        if (asprintf(&ssp.unit_name_prefix, "%s-virtiofsd-%u", scope_prefix, virtiofsd_instance++) < 0)
                return log_oom();

        state_dir = path_join(arg_runtime_directory, ssp.unit_name_prefix);
        if (!state_dir)
                return log_oom();

        if (arg_runtime_directory_created) {
                ssp.runtime_directory = strjoin("systemd/vmspawn/", ssp.unit_name_prefix);
                if (!ssp.runtime_directory)
                        return log_oom();
        }

        if (asprintf(&sock_name, "sock-%"PRIx64, random_u64()) < 0)
                return log_oom();

        ssp.listen_address = path_join(state_dir, sock_name);
        if (!ssp.listen_address)
                return log_oom();

        /* QEMU doesn't support submounts so don't announce them */
        ssp.exec_start = strv_new(virtiofsd, "--shared-dir", directory, "--xattr", "--fd", "3", "--no-announce-submounts");
        if (!ssp.exec_start)
                return log_oom();

        if (uidmap && arg_uid_shift != UID_INVALID) {
                r = strv_extend(&ssp.exec_start, "--uid-map");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&ssp.exec_start, ":0:" UID_FMT ":" UID_FMT ":", arg_uid_shift, arg_uid_range);
                if (r < 0)
                        return log_oom();

                r = strv_extend(&ssp.exec_start, "--gid-map");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&ssp.exec_start, ":0:" GID_FMT ":" GID_FMT ":", arg_uid_shift, arg_uid_range);
                if (r < 0)
                        return log_oom();
        }

        r = start_socket_service_pair(bus, scope, &ssp);
        if (r < 0)
                return r;

        *ret_state_tempdir = TAKE_PTR(state_dir);
        *ret_sock_name = TAKE_PTR(sock_name);

        return 0;
}

static int kernel_cmdline_maybe_append_root(void) {
        int r;
        bool cmdline_contains_root = strv_find_startswith(arg_kernel_cmdline_extra, "root=")
                        || strv_find_startswith(arg_kernel_cmdline_extra, "mount.usr=");

        if (!cmdline_contains_root) {
                _cleanup_free_ char *root = NULL;

                r = discover_root(&root);
                if (r < 0)
                        return r;

                log_debug("Determined root file system %s from dissected image", root);

                r = strv_consume(&arg_kernel_cmdline_extra, TAKE_PTR(root));
                if (r < 0)
                        return log_oom();
        }

        return 0;
}

static int discover_boot_entry(const char *root, char **ret_linux, char ***ret_initrds) {
        _cleanup_(boot_config_free) BootConfig config = BOOT_CONFIG_NULL;
        _cleanup_free_ char *esp_path = NULL, *xbootldr_path = NULL;
        int r;

        assert(root);
        assert(ret_linux);
        assert(ret_initrds);

        esp_path = path_join(root, "efi");
        if (!esp_path)
                return log_oom();

        xbootldr_path = path_join(root, "boot");
        if (!xbootldr_path)
                return log_oom();

        r = boot_config_load(&config, esp_path, xbootldr_path);
        if (r < 0)
                return r;

        r = boot_config_select_special_entries(&config, /* skip_efivars= */ true);
        if (r < 0)
                return log_error_errno(r, "Failed to find special boot config entries: %m");

        const BootEntry *boot_entry = boot_config_default_entry(&config);

        if (boot_entry && !IN_SET(boot_entry->type, BOOT_ENTRY_UNIFIED, BOOT_ENTRY_CONF))
                boot_entry = NULL;

        /* If we cannot determine a default entry search for UKIs (Type #2 EFI Unified Kernel Images)
         * then .conf files (Type #1 Boot Loader Specification Entries).
         * https://uapi-group.org/specifications/specs/boot_loader_specification */
        if (!boot_entry)
                FOREACH_ARRAY(entry, config.entries, config.n_entries)
                        if (entry->type == BOOT_ENTRY_UNIFIED) {
                                boot_entry = entry;
                                break;
                        }

        if (!boot_entry)
                FOREACH_ARRAY(entry, config.entries, config.n_entries)
                        if (entry->type == BOOT_ENTRY_CONF) {
                                boot_entry = entry;
                                break;
                        }

        if (!boot_entry)
                return log_error_errno(SYNTHETIC_ERRNO(ENOENT), "Failed to discover any boot entries.");

        log_debug("Discovered boot entry %s (%s)", boot_entry->id, boot_entry_type_to_string(boot_entry->type));

        _cleanup_free_ char *linux_kernel = NULL;
        _cleanup_strv_free_ char **initrds = NULL;
        if (boot_entry->type == BOOT_ENTRY_UNIFIED) {
                linux_kernel = path_join(boot_entry->root, boot_entry->kernel);
                if (!linux_kernel)
                        return log_oom();
        } else if (boot_entry->type == BOOT_ENTRY_CONF) {
                linux_kernel = path_join(boot_entry->root, boot_entry->kernel);
                if (!linux_kernel)
                        return log_oom();

                STRV_FOREACH(initrd, boot_entry->initrd) {
                        _cleanup_free_ char *initrd_path = path_join(boot_entry->root, *initrd);
                        if (!initrd_path)
                                return log_oom();

                        r = strv_consume(&initrds, TAKE_PTR(initrd_path));
                        if (r < 0)
                                return log_oom();
                }
        } else
                assert_not_reached();

        *ret_linux = TAKE_PTR(linux_kernel);
        *ret_initrds = TAKE_PTR(initrds);

        return 0;
}

static int merge_initrds(char **ret) {
        _cleanup_(rm_rf_physical_and_freep) char *merged_initrd = NULL;
        _cleanup_close_ int ofd = -EBADF;
        int r;

        assert(ret);

        r = tempfn_random_child(NULL, "vmspawn-initrd-", &merged_initrd);
        if (r < 0)
                return log_error_errno(r, "Failed to create temporary file: %m");

        ofd = open(merged_initrd, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0600);
        if (ofd < 0)
                return log_error_errno(errno, "Failed to create regular file %s: %m", merged_initrd);

        STRV_FOREACH(i, arg_initrds) {
                _cleanup_close_ int ifd = -EBADF;
                off_t off, to_seek;

                off = lseek(ofd, 0, SEEK_CUR);
                if (off < 0)
                        return log_error_errno(errno, "Failed to get file offset of %s: %m", merged_initrd);

                to_seek = (4 - (off % 4)) % 4;

                /* seek to assure 4 byte alignment for each initrd */
                if (to_seek != 0 && lseek(ofd, to_seek, SEEK_CUR) < 0)
                        return log_error_errno(errno, "Failed to seek %s: %m", merged_initrd);

                ifd = open(*i, O_RDONLY|O_CLOEXEC);
                if (ifd < 0)
                        return log_error_errno(errno, "Failed to open %s: %m", *i);

                r = copy_bytes(ifd, ofd, UINT64_MAX, COPY_REFLINK);
                if (r < 0)
                        return log_error_errno(r, "Failed to copy bytes from %s to %s: %m", *i, merged_initrd);
        }

        *ret = TAKE_PTR(merged_initrd);
        return 0;
}

static int run_virtual_machine(int kvm_device_fd, int vhost_device_fd) {
        _cleanup_(ovmf_config_freep) OvmfConfig *ovmf_config = NULL;
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        _cleanup_free_ char *machine = NULL, *qemu_binary = NULL, *mem = NULL, *trans_scope = NULL, *kernel = NULL;
        _cleanup_close_ int notify_sock_fd = -EBADF;
        _cleanup_strv_free_ char **cmdline = NULL;
        _cleanup_free_ int *pass_fds = NULL;
        size_t n_pass_fds = 0;
        const char *accel, *shm;
        int r;

        if (arg_privileged)
                r = sd_bus_default_system(&bus);
        else
                r = sd_bus_default_user(&bus);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to systemd bus: %m");

        r = start_transient_scope(bus, arg_machine, /* allow_pidfd= */ true, &trans_scope);
        if (r < 0)
                return r;

        bool use_kvm = arg_qemu_kvm > 0;
        if (arg_qemu_kvm < 0) {
                r = qemu_check_kvm_support();
                if (r < 0)
                        return log_error_errno(r, "Failed to check for KVM support: %m");
                use_kvm = r;
        }

        if (arg_firmware)
                r = load_ovmf_config(arg_firmware, &ovmf_config);
        else
                r = find_ovmf_config(arg_secure_boot, &ovmf_config);
        if (r < 0)
                return log_error_errno(r, "Failed to find OVMF config: %m");

        /* only warn if the user hasn't disabled secureboot */
        if (!ovmf_config->supports_sb && arg_secure_boot)
                log_warning("Couldn't find OVMF firmware blob with Secure Boot support, "
                            "falling back to OVMF firmware blobs without Secure Boot support.");

        shm = arg_directory ? ",memory-backend=mem" : "";
        if (ARCHITECTURE_SUPPORTS_SMM)
                machine = strjoin("type=" QEMU_MACHINE_TYPE ",smm=", on_off(ovmf_config->supports_sb), shm);
        else
                machine = strjoin("type=" QEMU_MACHINE_TYPE, shm);
        if (!machine)
                return log_oom();

        if (arg_linux) {
                kernel = strdup(arg_linux);
                if (!kernel)
                        return log_oom();
        } else if (arg_directory) {
                /* a kernel is required for directory type images so attempt to locate a UKI under /boot and /efi */
                r = discover_boot_entry(arg_directory, &kernel, &arg_initrds);
                if (r < 0)
                        return log_error_errno(r, "Failed to locate UKI in directory type image, please specify one with --linux=.");

                log_debug("Discovered UKI image at %s", kernel);
        }

        r = find_qemu_binary(&qemu_binary);
        if (r == -EOPNOTSUPP)
                return log_error_errno(r, "Native architecture is not supported by qemu.");
        if (r < 0)
                return log_error_errno(r, "Failed to find QEMU binary: %m");

        if (asprintf(&mem, "%" PRIu64 "M", DIV_ROUND_UP(arg_qemu_mem, U64_MB)) < 0)
                return log_oom();

        cmdline = strv_new(
                qemu_binary,
                "-machine", machine,
                "-smp", arg_qemu_smp ?: "1",
                "-m", mem,
                "-object", "rng-random,filename=/dev/urandom,id=rng0",
                "-device", "virtio-rng-pci,rng=rng0,id=rng-device0"
        );
        if (!cmdline)
                return log_oom();

        /* if we are going to be starting any units with state then create our runtime dir */
        if (arg_tpm != 0 || arg_directory || arg_runtime_mounts.n_mounts != 0) {
                r = runtime_directory(&arg_runtime_directory, arg_privileged ? RUNTIME_SCOPE_SYSTEM : RUNTIME_SCOPE_USER, "systemd/vmspawn");
                if (r < 0)
                        return log_error_errno(r, "Failed to lookup runtime directory: %m");
                if (r) {
                        /* r > 0 means we need to create our own runtime dir */
                        r = mkdir_p(arg_runtime_directory, 0755);
                        if (r < 0)
                                return log_error_errno(r, "Failed to create runtime directory: %m");
                        arg_runtime_directory_created = true;
                }
        }

        if (arg_network_stack == QEMU_NET_TAP)
                r = strv_extend_many(&cmdline, "-nic", "tap,script=no,model=virtio-net-pci");
        else if (arg_network_stack == QEMU_NET_USER)
                r = strv_extend_many(&cmdline, "-nic", "user,model=virtio-net-pci");
        else
                r = strv_extend_many(&cmdline, "-nic", "none");
        if (r < 0)
                return log_oom();

        /* A shared memory backend might increase ram usage so only add one if actually necessary for virtiofsd. */
        if (arg_directory || arg_runtime_mounts.n_mounts != 0) {
                r = strv_extend(&cmdline, "-object");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&cmdline, "memory-backend-memfd,id=mem,size=%s,share=on", mem);
                if (r < 0)
                        return log_oom();
        }

        bool use_vsock = arg_qemu_vsock > 0 && ARCHITECTURE_SUPPORTS_SMBIOS;
        if (arg_qemu_vsock < 0) {
                r = qemu_check_vsock_support();
                if (r < 0)
                        return log_error_errno(r, "Failed to check for VSock support: %m");

                use_vsock = r;
        }

        if (!use_kvm && kvm_device_fd >= 0) {
                log_warning("KVM is disabled but fd for /dev/kvm was passed, closing fd and ignoring");
                kvm_device_fd = safe_close(kvm_device_fd);
        }

        if (use_kvm && kvm_device_fd >= 0) {
                /* /dev/fdset/1 is magic string to tell qemu where to find the fd for /dev/kvm
                 * we use this so that we can take a fd to /dev/kvm and then give qemu that fd */
                accel = "kvm,device=/dev/fdset/1";

                r = strv_extend(&cmdline, "--add-fd");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&cmdline, "fd=%d,set=1,opaque=/dev/kvm", kvm_device_fd);
                if (r < 0)
                        return log_oom();

                if (!GREEDY_REALLOC(pass_fds, n_pass_fds + 1))
                        return log_oom();

                pass_fds[n_pass_fds++] = kvm_device_fd;
        } else if (use_kvm)
                accel = "kvm";
        else
                accel = "tcg";

        r = strv_extend_many(&cmdline, "-accel", accel);
        if (r < 0)
                return log_oom();

        _cleanup_close_ int child_vsock_fd = -EBADF;
        unsigned child_cid = arg_vsock_cid;
        if (use_vsock) {
                int device_fd = vhost_device_fd;

                if (device_fd < 0) {
                        child_vsock_fd = open("/dev/vhost-vsock", O_RDWR|O_CLOEXEC);
                        if (child_vsock_fd < 0)
                                return log_error_errno(errno, "Failed to open /dev/vhost-vsock as read/write: %m");

                        device_fd = child_vsock_fd;
                }

                r = vsock_fix_child_cid(device_fd, &child_cid, arg_machine);
                if (r < 0)
                        return log_error_errno(r, "Failed to fix CID for the guest vsock socket: %m");

                r = strv_extend(&cmdline, "-device");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&cmdline, "vhost-vsock-pci,guest-cid=%u,vhostfd=%d", child_cid, device_fd);
                if (r < 0)
                        return log_oom();

                if (!GREEDY_REALLOC(pass_fds, n_pass_fds + 1))
                        return log_oom();

                pass_fds[n_pass_fds++] = device_fd;
        }

        r = strv_extend_many(&cmdline, "-cpu", "max");
        if (r < 0)
                return log_oom();

        if (arg_qemu_gui)
                r = strv_extend_many(
                                &cmdline,
                                "-vga",
                                "virtio");
        else
                r = strv_extend_many(
                                &cmdline,
                                "-nographic",
                                "-nodefaults",
                                "-chardev", "stdio,mux=on,id=console,signal=off",
                                "-serial", "chardev:console",
                                "-mon", "console");
        if (r < 0)
                return log_oom();

        r = strv_extend(&cmdline, "-drive");
        if (r < 0)
                return log_oom();

        r = strv_extendf(&cmdline, "if=pflash,format=%s,readonly=on,file=%s", ovmf_config_format(ovmf_config), ovmf_config->path);
        if (r < 0)
                return log_oom();

        _cleanup_(unlink_and_freep) char *ovmf_vars_to = NULL;
        if (ovmf_config->supports_sb) {
                const char *ovmf_vars_from = ovmf_config->vars;
                _cleanup_close_ int source_fd = -EBADF, target_fd = -EBADF;

                r = tempfn_random_child(NULL, "vmspawn-", &ovmf_vars_to);
                if (r < 0)
                        return r;

                source_fd = open(ovmf_vars_from, O_RDONLY|O_CLOEXEC);
                if (source_fd < 0)
                        return log_error_errno(source_fd, "Failed to open OVMF vars file %s: %m", ovmf_vars_from);

                target_fd = open(ovmf_vars_to, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0600);
                if (target_fd < 0)
                        return log_error_errno(errno, "Failed to create regular file for OVMF vars at %s: %m", ovmf_vars_to);

                r = copy_bytes(source_fd, target_fd, UINT64_MAX, COPY_REFLINK);
                if (r < 0)
                        return log_error_errno(r, "Failed to copy bytes from %s to %s: %m", ovmf_vars_from, ovmf_vars_to);

                /* These aren't always available so don't raise an error if they fail */
                (void) copy_xattr(source_fd, NULL, target_fd, NULL, 0);
                (void) copy_access(source_fd, target_fd);
                (void) copy_times(source_fd, target_fd, 0);

                r = strv_extend_many(
                                &cmdline,
                                "-global", "ICH9-LPC.disable_s3=1",
                                "-global", "driver=cfi.pflash01,property=secure,value=on",
                                "-drive");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&cmdline, "file=%s,if=pflash,format=%s", ovmf_vars_to, ovmf_config_format(ovmf_config));
                if (r < 0)
                        return log_oom();
        }

        if (kernel) {
                r = strv_extend_many(&cmdline, "-kernel", kernel);
                if (r < 0)
                        return log_oom();

                /* We can't rely on gpt-auto-generator when direct kernel booting so synthesize a root=
                 * kernel argument instead. */
                if (arg_image) {
                        r = kernel_cmdline_maybe_append_root();
                        if (r < 0)
                                return r;
                }
        }

        if (arg_image) {
                assert(!arg_directory);

                r = strv_extend(&cmdline, "-drive");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&cmdline, "if=none,id=mkosi,file=%s,format=raw", arg_image);
                if (r < 0)
                        return log_oom();

                r = strv_extend_many(&cmdline,
                        "-device", "virtio-scsi-pci,id=scsi",
                        "-device", "scsi-hd,drive=mkosi,bootindex=1");
                if (r < 0)
                        return log_oom();
        }

        if (arg_directory) {
                _cleanup_free_ char *sock_path = NULL, *sock_name = NULL;
                r = start_virtiofsd(bus, trans_scope, arg_directory, /* uidmap= */ true, &sock_path, &sock_name);
                if (r < 0)
                        return r;

                r = strv_extend(&cmdline, "-chardev");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&cmdline, "socket,id=%1$s,path=%2$s/%1$s", sock_name, sock_path);
                if (r < 0)
                        return log_oom();

                r = strv_extend(&cmdline, "-device");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&cmdline, "vhost-user-fs-pci,queue-size=1024,chardev=%s,tag=root", sock_name);
                if (r < 0)
                        return log_oom();

                r = strv_extend(&arg_kernel_cmdline_extra, "root=root rootfstype=virtiofs rw");
                if (r < 0)
                        return log_oom();
        }

        r = strv_prepend(&arg_kernel_cmdline_extra, "console=" DEFAULT_SERIAL_TTY);
        if (r < 0)
                return log_oom();

        FOREACH_ARRAY(mount, arg_runtime_mounts.mounts, arg_runtime_mounts.n_mounts) {
                _cleanup_free_ char *sock_path = NULL, *sock_name = NULL, *clean_target = NULL;
                r = start_virtiofsd(bus, trans_scope, mount->source, /* uidmap= */ false, &sock_path, &sock_name);
                if (r < 0)
                        return r;

                r = strv_extend(&cmdline, "-chardev");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&cmdline, "socket,id=%1$s,path=%2$s/%1$s", sock_name, sock_path);
                if (r < 0)
                        return log_oom();

                r = strv_extend(&cmdline, "-device");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&cmdline, "vhost-user-fs-pci,queue-size=1024,chardev=%1$s,tag=%1$s", sock_name);
                if (r < 0)
                        return log_oom();

                clean_target = xescape(mount->target, "\":");
                if (!clean_target)
                        return log_oom();

                r = strv_extendf(&arg_kernel_cmdline_extra, "systemd.mount-extra=\"%s:%s:virtiofs:%s\"",
                                 sock_name, clean_target, mount->read_only ? "ro" : "rw");
                if (r < 0)
                        return log_oom();
        }

        if (ARCHITECTURE_SUPPORTS_SMBIOS) {
                _cleanup_free_ char *kcl = strv_join(arg_kernel_cmdline_extra, " ");
                if (!kcl)
                        return log_oom();

                if (kernel) {
                        r = strv_extend_many(&cmdline, "-append", kcl);
                        if (r < 0)
                                return log_oom();
                } else {
                        if (ARCHITECTURE_SUPPORTS_SMBIOS) {
                                r = strv_extend(&cmdline, "-smbios");
                                if (r < 0)
                                        return log_oom();

                                r = strv_extendf(&cmdline, "type=11,value=io.systemd.stub.kernel-cmdline-extra=%s", kcl);
                                if (r < 0)
                                        return log_oom();
                        } else
                                log_warning("Cannot append extra args to kernel cmdline, native architecture doesn't support SMBIOS, ignoring");
                }
        } else
                log_warning("Cannot append extra args to kernel cmdline, native architecture doesn't support SMBIOS");

        /* disable TPM autodetection if the user's hardware doesn't support it */
        if (!ARCHITECTURE_SUPPORTS_TPM) {
                if (arg_tpm < 0) {
                        arg_tpm = 0;
                        log_debug("TPM not support on %s, disabling tpm autodetection and continuing", architecture_to_string(native_architecture()));
                } else if (arg_tpm > 0)
                        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "TPM not supported on %s, refusing", architecture_to_string(native_architecture()));
        }

        _cleanup_free_ char *swtpm = NULL;
        if (arg_tpm != 0) {
                r = find_executable("swtpm", &swtpm);
                if (r < 0) {
                        /* log if the user asked for swtpm and we cannot find it */
                        if (arg_tpm > 0)
                                return log_error_errno(r, "Failed to find swtpm binary: %m");
                        /* also log if we got an error other than ENOENT from find_executable */
                        if (r != -ENOENT && arg_tpm < 0)
                                return log_error_errno(r, "Error detecting swtpm: %m");
                }
        }

        _cleanup_free_ const char *tpm_state_tempdir = NULL;
        if (swtpm) {
                r = start_tpm(bus, trans_scope, swtpm, &tpm_state_tempdir);
                if (r < 0) {
                        /* only bail if the user asked for a tpm */
                        if (arg_tpm > 0)
                                return log_error_errno(r, "Failed to start tpm: %m");
                        log_debug_errno(r, "Failed to start tpm, ignoring: %m");
                }

                r = strv_extend(&cmdline, "-chardev");
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&cmdline, "socket,id=chrtpm,path=%s/sock", tpm_state_tempdir);
                if (r < 0)
                        return log_oom();

                r = strv_extend_many(&cmdline, "-tpmdev", "emulator,id=tpm0,chardev=chrtpm");
                if (r < 0)
                        return log_oom();

                if (native_architecture() == ARCHITECTURE_X86_64)
                        r = strv_extend_many(&cmdline, "-device", "tpm-tis,tpmdev=tpm0");
                else if (IN_SET(native_architecture(), ARCHITECTURE_ARM64, ARCHITECTURE_ARM64_BE))
                        r = strv_extend_many(&cmdline, "-device", "tpm-tis-device,tpmdev=tpm0");
                if (r < 0)
                        return log_oom();
        }

        char *initrd = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *merged_initrd = NULL;
        size_t n_initrds = strv_length(arg_initrds);

        if (n_initrds == 1)
                initrd = arg_initrds[0];
        else if (n_initrds > 1) {
                r = merge_initrds(&merged_initrd);
                if (r < 0)
                        return r;

                initrd = merged_initrd;
        }

        if (initrd) {
                r = strv_extend_many(&cmdline, "-initrd", initrd);
                if (r < 0)
                        return log_oom();
        }

        if (arg_forward_journal) {
                _cleanup_free_ char *sd_journal_remote = NULL, *listen_address = NULL, *cred = NULL;
                r = find_executable("systemd-journal-remote", &sd_journal_remote);
                if (r < 0)
                        return log_error_errno(r, "Failed to find systemd-journal-remote binary: %m");

                r = start_systemd_journal_remote(bus, trans_scope, child_cid, sd_journal_remote, &listen_address);
                if (r < 0)
                        return r;

                cred = strjoin("journal.forward_to_socket:", listen_address);
                if (!cred)
                        return log_oom();

                r = machine_credential_set(&arg_credentials, cred);
                if (r < 0)
                        return r;
        }

        if (ARCHITECTURE_SUPPORTS_SMBIOS)
                FOREACH_ARRAY(cred, arg_credentials.credentials, arg_credentials.n_credentials) {
                        _cleanup_free_ char *cred_data_b64 = NULL;
                        ssize_t n;

                        n = base64mem(cred->data, cred->size, &cred_data_b64);
                        if (n < 0)
                                return log_oom();

                        r = strv_extend(&cmdline, "-smbios");
                        if (r < 0)
                                return log_oom();

                        r = strv_extendf(&cmdline, "type=11,value=io.systemd.credential.binary:%s=%s", cred->id, cred_data_b64);
                        if (r < 0)
                                return log_oom();
                }

        if (use_vsock) {
                notify_sock_fd = open_vsock();
                if (notify_sock_fd < 0)
                        return log_error_errno(notify_sock_fd, "Failed to open vsock: %m");

                r = cmdline_add_vsock(&cmdline, notify_sock_fd);
                if (r == -ENOMEM)
                        return log_oom();
                if (r < 0)
                        return log_error_errno(r, "Failed to call getsockname on vsock: %m");
        }

        if (DEBUG_LOGGING) {
                _cleanup_free_ char *joined = quote_command_line(cmdline, SHELL_ESCAPE_EMPTY);
                if (!joined)
                        return log_oom();

                log_debug("Executing: %s", joined);
        }

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGCHLD, -1) >= 0);

        _cleanup_(sd_event_source_unrefp) sd_event_source *notify_event_source = NULL;
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        r = sd_event_new(&event);
        if (r < 0)
                return log_error_errno(r, "Failed to get default event source: %m");

        (void) sd_event_set_watchdog(event, true);

        _cleanup_(pidref_done) PidRef child_pidref = PIDREF_NULL;

        r = pidref_safe_fork_full(
                        qemu_binary,
                        /* stdio_fds= */ NULL,
                        &child_vsock_fd, 1, /* pass the vsock fd to qemu */
                        FORK_RESET_SIGNALS|FORK_CLOSE_ALL_FDS|FORK_DEATHSIG_SIGTERM|FORK_LOG|FORK_CLOEXEC_OFF|FORK_RLIMIT_NOFILE_SAFE,
                        &child_pidref);
        if (r < 0)
                return r;
        if (r == 0) {
                /* set TERM and LANG if they are missing */
                if (setenv("TERM", "vt220", 0) < 0)
                        return log_oom();

                if (setenv("LANG", "C.UTF-8", 0) < 0)
                        return log_oom();

                execv(qemu_binary, cmdline);
                log_error_errno(errno, "Failed to execve %s: %m", qemu_binary);
                _exit(EXIT_FAILURE);
        }

        /* Close the vsock fd we passed to qemu in the parent. We don't need it anymore. */
        child_vsock_fd = safe_close(child_vsock_fd);

        int exit_status = INT_MAX;
        if (use_vsock) {
                r = setup_notify_parent(event, notify_sock_fd, &exit_status, &notify_event_source);
                if (r < 0)
                        return log_error_errno(r, "Failed to setup event loop to handle vsock notify events: %m");
        }

        /* shutdown qemu when we are shutdown */
        (void) sd_event_add_signal(event, NULL, SIGINT | SD_EVENT_SIGNAL_PROCMASK, on_orderly_shutdown, &child_pidref);
        (void) sd_event_add_signal(event, NULL, SIGTERM | SD_EVENT_SIGNAL_PROCMASK, on_orderly_shutdown, &child_pidref);

        (void) sd_event_add_signal(event, NULL, (SIGRTMIN+18) | SD_EVENT_SIGNAL_PROCMASK, sigrtmin18_handler, NULL);

        /* Exit when the child exits */
        (void) event_add_child_pidref(event, NULL, &child_pidref, WEXITED, on_child_exit, NULL);

        r = sd_event_loop(event);
        if (r < 0)
                return log_error_errno(r, "Failed to run event loop: %m");

        if (use_vsock) {
                if (exit_status == INT_MAX) {
                        log_debug("Couldn't retrieve inner EXIT_STATUS from vsock");
                        return EXIT_SUCCESS;
                }
                if (exit_status != 0)
                        log_warning("Non-zero exit code received: %d", exit_status);
                return exit_status;
        }

        return 0;
}

static int determine_names(void) {
        int r;

        if (!arg_directory && !arg_image) {
                if (arg_machine) {
                        _cleanup_(image_unrefp) Image *i = NULL;

                        r = image_find(IMAGE_MACHINE, arg_machine, NULL, &i);
                        if (r == -ENOENT)
                                return log_error_errno(r, "No image for machine '%s'.", arg_machine);
                        if (r < 0)
                                return log_error_errno(r, "Failed to find image for machine '%s': %m", arg_machine);

                        if (IN_SET(i->type, IMAGE_RAW, IMAGE_BLOCK))
                                r = free_and_strdup(&arg_image, i->path);
                        else if (IN_SET(i->type, IMAGE_DIRECTORY, IMAGE_SUBVOLUME))
                                r = free_and_strdup(&arg_directory, i->path);
                        else
                                assert_not_reached();
                        if (r < 0)
                                return log_oom();
                } else {
                        r = safe_getcwd(&arg_directory);
                        if (r < 0)
                                return log_error_errno(r, "Failed to determine current directory: %m");
                }
        }

        if (!arg_machine) {
                if (arg_directory && path_equal(arg_directory, "/")) {
                        arg_machine = gethostname_malloc();
                        if (!arg_machine)
                                return log_oom();
                } else if (arg_image) {
                        char *e;

                        r = path_extract_filename(arg_image, &arg_machine);
                        if (r < 0)
                                return log_error_errno(r, "Failed to extract file name from '%s': %m", arg_image);

                        /* Truncate suffix if there is one */
                        e = endswith(arg_machine, ".raw");
                        if (e)
                                *e = 0;
                } else {
                        r = path_extract_filename(arg_directory, &arg_machine);
                        if (r < 0)
                                return log_error_errno(r, "Failed to extract file name from '%s': %m", arg_directory);
                }

                hostname_cleanup(arg_machine);
                if (!hostname_is_valid(arg_machine, 0))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Failed to determine machine name automatically, please use -M.");
        }

        return 0;
}

static int verify_arguments(void) {
        if (arg_network_stack == QEMU_NET_TAP && !arg_privileged)
                return log_error_errno(SYNTHETIC_ERRNO(EPERM), "--network-tap requires root privileges, refusing.");

        if (!strv_isempty(arg_initrds) && !arg_linux)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Option --initrd= cannot be used without --linux=.");

        return 0;
}

static int run(int argc, char *argv[]) {
        int r, kvm_device_fd = -EBADF, vhost_device_fd = -EBADF;
        _cleanup_strv_free_ char **names = NULL;

        log_setup();

        arg_privileged = getuid() == 0;

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        r = determine_names();
        if (r < 0)
                return r;

        r = verify_arguments();
        if (r < 0)
                return r;

        if (!arg_quiet) {
                _cleanup_free_ char *u = NULL;
                const char *vm_path = arg_image ?: arg_directory;
                (void) terminal_urlify_path(vm_path, vm_path, &u);

                log_info("%s %sSpawning VM %s on %s.%s\n"
                         "%s %sPress %sCtrl-a x%s to kill VM.%s",
                         special_glyph(SPECIAL_GLYPH_LIGHT_SHADE), ansi_grey(), arg_machine, u ?: vm_path, ansi_normal(),
                         special_glyph(SPECIAL_GLYPH_LIGHT_SHADE), ansi_grey(), ansi_highlight(), ansi_grey(), ansi_normal());
        }

        r = sd_listen_fds_with_names(true, &names);
        if (r < 0)
                return log_error_errno(r, "Failed to get passed file descriptors: %m");

        for (int i = 0; i < r; i++) {
                int fd = SD_LISTEN_FDS_START + i;
                if (streq(names[i], "kvm"))
                        kvm_device_fd = fd;
                else if (streq(names[i], "vhost-vsock"))
                        vhost_device_fd = fd;
                else {
                        log_notice("Couldn't recognize passed fd %d (%s), closing fd and ignoring...", fd, names[i]);
                        safe_close(fd);
                }
        }

        return run_virtual_machine(kvm_device_fd, vhost_device_fd);
}

DEFINE_MAIN_FUNCTION_WITH_POSITIVE_FAILURE(run);
