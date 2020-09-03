/* SPDX-License-Identifier: LGPL-2.1+ */

#include "alloc-util.h"
#include "missing_network.h"
#include "tests.h"
#include "socket-netlink.h"
#include "string-util.h"

static void test_socket_address_parse_one(const char *in, int ret, int family, const char *expected) {
        SocketAddress a;
        _cleanup_free_ char *out = NULL;
        int r;

        r = socket_address_parse(&a, in);
        if (r >= 0) {
                r = socket_address_print(&a, &out);
                if (r < 0)
                        log_error_errno(r, "Printing failed for \"%s\": %m", in);
                assert(r >= 0);
                assert_se(a.type == 0);
        }

        log_info("\"%s\" → %s %d → \"%s\" (expect %d / \"%s\")",
                 in,
                 r >= 0 ? "✓" : "✗", r,
                 empty_to_dash(out),
                 ret,
                 ret >= 0 ? expected ?: in : "-");
        assert_se(r == ret);
        if (r >= 0) {
                assert_se(a.sockaddr.sa.sa_family == family);
                assert_se(streq(out, expected ?: in));
        }
}

static void test_socket_address_parse(void) {
        log_info("/* %s */", __func__);

        test_socket_address_parse_one("junk", -EINVAL, 0, NULL);
        test_socket_address_parse_one("192.168.1.1", -EINVAL, 0, NULL);
        test_socket_address_parse_one(".168.1.1", -EINVAL, 0, NULL);
        test_socket_address_parse_one("989.168.1.1", -EINVAL, 0, NULL);
        test_socket_address_parse_one("192.168.1.1:65536", -ERANGE, 0, NULL);
        test_socket_address_parse_one("192.168.1.1:0", -EINVAL, 0, NULL);
        test_socket_address_parse_one("0", -EINVAL, 0, NULL);
        test_socket_address_parse_one("65536", -ERANGE, 0, NULL);

        const int default_family = socket_ipv6_is_supported() ? AF_INET6 : AF_INET;

        test_socket_address_parse_one("65535", 0, default_family, "[::]:65535");

        /* The checks below will pass even if ipv6 is disabled in
         * kernel. The underlying glibc's inet_pton() is just a string
         * parser and doesn't make any syscalls. */

        test_socket_address_parse_one("[::1]", -EINVAL, 0, NULL);
        test_socket_address_parse_one("[::1]8888", -EINVAL, 0, NULL);
        test_socket_address_parse_one("::1", -EINVAL, 0, NULL);
        test_socket_address_parse_one("[::1]:0", -EINVAL, 0, NULL);
        test_socket_address_parse_one("[::1]:65536", -ERANGE, 0, NULL);
        test_socket_address_parse_one("[a:b:1]:8888", -EINVAL, 0, NULL);

        test_socket_address_parse_one("8888", 0, default_family, "[::]:8888");
        test_socket_address_parse_one("[2001:0db8:0000:85a3:0000:0000:ac1f:8001]:8888", 0, AF_INET6,
                                      "[2001:db8:0:85a3::ac1f:8001]:8888");
        test_socket_address_parse_one("[::1]:8888", 0, AF_INET6, NULL);
        test_socket_address_parse_one("192.168.1.254:8888", 0, AF_INET, NULL);
        test_socket_address_parse_one("/foo/bar", 0, AF_UNIX, NULL);
        test_socket_address_parse_one("/", 0, AF_UNIX, NULL);
        test_socket_address_parse_one("@abstract", 0, AF_UNIX, NULL);

        {
                char aaa[SUN_PATH_LEN + 1] = "@";

                memset(aaa + 1, 'a', SUN_PATH_LEN - 1);
                char_array_0(aaa);

                test_socket_address_parse_one(aaa, -EINVAL, 0, NULL);

                aaa[SUN_PATH_LEN - 1] = '\0';
                test_socket_address_parse_one(aaa, 0, AF_UNIX, NULL);
        }

        test_socket_address_parse_one("vsock:2:1234", 0, AF_VSOCK, NULL);
        test_socket_address_parse_one("vsock::1234", 0, AF_VSOCK, NULL);
        test_socket_address_parse_one("vsock:2:1234x", -EINVAL, 0, NULL);
        test_socket_address_parse_one("vsock:2x:1234", -EINVAL, 0, NULL);
        test_socket_address_parse_one("vsock:2", -EINVAL, 0, NULL);
}

static void test_socket_address_parse_netlink(void) {
        SocketAddress a;

        log_info("/* %s */", __func__);

        assert_se(socket_address_parse_netlink(&a, "junk") < 0);
        assert_se(socket_address_parse_netlink(&a, "") < 0);

        assert_se(socket_address_parse_netlink(&a, "route") >= 0);
        assert_se(a.sockaddr.nl.nl_family == AF_NETLINK);
        assert_se(a.sockaddr.nl.nl_groups == 0);
        assert_se(a.protocol == NETLINK_ROUTE);
        assert_se(socket_address_parse_netlink(&a, "route") >= 0);
        assert_se(socket_address_parse_netlink(&a, "route 10") >= 0);
        assert_se(a.sockaddr.nl.nl_family == AF_NETLINK);
        assert_se(a.sockaddr.nl.nl_groups == 10);
        assert_se(a.protocol == NETLINK_ROUTE);

        /* With spaces and tabs */
        assert_se(socket_address_parse_netlink(&a, " kobject-uevent ") >= 0);
        assert_se(a.sockaddr.nl.nl_family == AF_NETLINK);
        assert_se(a.sockaddr.nl.nl_groups == 0);
        assert_se(a.protocol == NETLINK_KOBJECT_UEVENT);
        assert_se(socket_address_parse_netlink(&a, " \t kobject-uevent \t 10") >= 0);
        assert_se(a.sockaddr.nl.nl_family == AF_NETLINK);
        assert_se(a.sockaddr.nl.nl_groups == 10);
        assert_se(a.protocol == NETLINK_KOBJECT_UEVENT);
        assert_se(socket_address_parse_netlink(&a, "kobject-uevent\t10") >= 0);
        assert_se(a.sockaddr.nl.nl_family == AF_NETLINK);
        assert_se(a.sockaddr.nl.nl_groups == 10);
        assert_se(a.protocol == NETLINK_KOBJECT_UEVENT);

        /* trailing space is not supported */
        assert_se(socket_address_parse_netlink(&a, "kobject-uevent\t10 ") < 0);

        /* Group must be unsigned */
        assert_se(socket_address_parse_netlink(&a, "kobject-uevent -1") < 0);

        /* oss-fuzz #6884 */
        assert_se(socket_address_parse_netlink(&a, "\xff") < 0);
}

static void test_socket_address_equal(void) {
        SocketAddress a, b;

        log_info("/* %s */", __func__);

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_parse(&b, "192.168.1.1:888") >= 0);
        assert_se(!socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_parse(&b, "192.16.1.1:8888") >= 0);
        assert_se(!socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_parse(&b, "8888") >= 0);
        assert_se(!socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_parse(&b, "/foo/bar/") >= 0);
        assert_se(!socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_parse(&b, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "/foo/bar") >= 0);
        assert_se(socket_address_parse(&b, "/foo/bar") >= 0);
        assert_se(socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "[::1]:8888") >= 0);
        assert_se(socket_address_parse(&b, "[::1]:8888") >= 0);
        assert_se(socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "@abstract") >= 0);
        assert_se(socket_address_parse(&b, "@abstract") >= 0);
        assert_se(socket_address_equal(&a, &b));

        assert_se(socket_address_parse_netlink(&a, "firewall") >= 0);
        assert_se(socket_address_parse_netlink(&b, "firewall") >= 0);
        assert_se(socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "vsock:2:1234") >= 0);
        assert_se(socket_address_parse(&b, "vsock:2:1234") >= 0);
        assert_se(socket_address_equal(&a, &b));
        assert_se(socket_address_parse(&b, "vsock:2:1235") >= 0);
        assert_se(!socket_address_equal(&a, &b));
        assert_se(socket_address_parse(&b, "vsock:3:1234") >= 0);
        assert_se(!socket_address_equal(&a, &b));
}

static void test_socket_address_get_path(void) {
        SocketAddress a;

        log_info("/* %s */", __func__);

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(!socket_address_get_path(&a));

        assert_se(socket_address_parse(&a, "@abstract") >= 0);
        assert_se(!socket_address_get_path(&a));

        assert_se(socket_address_parse(&a, "[::1]:8888") >= 0);
        assert_se(!socket_address_get_path(&a));

        assert_se(socket_address_parse(&a, "/foo/bar") >= 0);
        assert_se(streq(socket_address_get_path(&a), "/foo/bar"));

        assert_se(socket_address_parse(&a, "vsock:2:1234") >= 0);
        assert_se(!socket_address_get_path(&a));
}

static void test_socket_address_is(void) {
        SocketAddress a;

        log_info("/* %s */", __func__);

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se( socket_address_is(&a, "192.168.1.1:8888", 0 /* unspecified yet */));
        assert_se(!socket_address_is(&a, "route", 0));
        assert_se(!socket_address_is(&a, "route", SOCK_STREAM));
        assert_se(!socket_address_is(&a, "192.168.1.1:8888", SOCK_RAW));
        assert_se(!socket_address_is(&a, "192.168.1.1:8888", SOCK_STREAM));
        a.type = SOCK_STREAM;
        assert_se( socket_address_is(&a, "192.168.1.1:8888", SOCK_STREAM));
}

static void test_socket_address_is_netlink(void) {
        SocketAddress a;

        log_info("/* %s */", __func__);

        assert_se(socket_address_parse_netlink(&a, "route 10") >= 0);
        assert_se( socket_address_is_netlink(&a, "route 10"));
        assert_se(!socket_address_is_netlink(&a, "192.168.1.1:8888"));
        assert_se(!socket_address_is_netlink(&a, "route 1"));
}

static void test_in_addr_ifindex_to_string_one(int f, const char *a, int ifindex, const char *b) {
        _cleanup_free_ char *r = NULL;
        union in_addr_union ua, uuaa;
        int ff, ifindex2;

        assert_se(in_addr_from_string(f, a, &ua) >= 0);
        assert_se(in_addr_ifindex_to_string(f, &ua, ifindex, &r) >= 0);
        printf("test_in_addr_ifindex_to_string_one: %s == %s\n", b, r);
        assert_se(streq(b, r));

        assert_se(in_addr_ifindex_from_string_auto(b, &ff, &uuaa, &ifindex2) >= 0);
        assert_se(ff == f);
        assert_se(in_addr_equal(f, &ua, &uuaa));
        assert_se(ifindex2 == ifindex || ifindex2 == 0);
}

static void test_in_addr_ifindex_to_string(void) {
        log_info("/* %s */", __func__);

        test_in_addr_ifindex_to_string_one(AF_INET, "192.168.0.1", 7, "192.168.0.1");
        test_in_addr_ifindex_to_string_one(AF_INET, "10.11.12.13", 9, "10.11.12.13");
        test_in_addr_ifindex_to_string_one(AF_INET6, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 10, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
        test_in_addr_ifindex_to_string_one(AF_INET6, "::1", 11, "::1");
        test_in_addr_ifindex_to_string_one(AF_INET6, "fe80::", 12, "fe80::%12");
        test_in_addr_ifindex_to_string_one(AF_INET6, "fe80::", 0, "fe80::");
        test_in_addr_ifindex_to_string_one(AF_INET6, "fe80::14", 12, "fe80::14%12");
        test_in_addr_ifindex_to_string_one(AF_INET6, "fe80::15", -7, "fe80::15");
        test_in_addr_ifindex_to_string_one(AF_INET6, "fe80::16", LOOPBACK_IFINDEX, "fe80::16%1");
}

static void test_in_addr_ifindex_from_string_auto(void) {
        int family, ifindex;
        union in_addr_union ua;

        log_info("/* %s */", __func__);
        /* Most in_addr_ifindex_from_string_auto() invocations have already been tested above, but let's test some more */

        assert_se(in_addr_ifindex_from_string_auto("fe80::17", &family, &ua, &ifindex) >= 0);
        assert_se(family == AF_INET6);
        assert_se(ifindex == 0);

        assert_se(in_addr_ifindex_from_string_auto("fe80::18%19", &family, &ua, &ifindex) >= 0);
        assert_se(family == AF_INET6);
        assert_se(ifindex == 19);

        assert_se(in_addr_ifindex_from_string_auto("fe80::18%lo", &family, &ua, &ifindex) >= 0);
        assert_se(family == AF_INET6);
        assert_se(ifindex == LOOPBACK_IFINDEX);

        assert_se(in_addr_ifindex_from_string_auto("fe80::19%thisinterfacecantexist", &family, &ua, &ifindex) == -ENODEV);
}

static void test_in_addr_ifindex_name_from_string_auto_one(const char *a, const char *expected) {
        int family, ifindex;
        union in_addr_union ua;
        _cleanup_free_ char *server_name = NULL;

        assert_se(in_addr_ifindex_name_from_string_auto(a, &family, &ua, &ifindex, &server_name) >= 0);
        assert_se(streq_ptr(server_name, expected));
}

static void test_in_addr_ifindex_name_from_string_auto(void) {
        log_info("/* %s */", __func__);

        test_in_addr_ifindex_name_from_string_auto_one("192.168.0.1", NULL);
        test_in_addr_ifindex_name_from_string_auto_one("192.168.0.1#test.com", "test.com");
        test_in_addr_ifindex_name_from_string_auto_one("fe80::18%19", NULL);
        test_in_addr_ifindex_name_from_string_auto_one("fe80::18%19#another.test.com", "another.test.com");
}

static void test_in_addr_port_ifindex_name_from_string_auto_one(const char *str, int family, uint16_t port, int ifindex, const char *server_name) {
        union in_addr_union a;
        uint16_t p;
        int f, i;
        char *fake;

        log_info("%s: %s", __func__, str);

        {
                _cleanup_free_ char *name = NULL, *x = NULL;
                assert_se(in_addr_port_ifindex_name_from_string_auto(str, &f, &a, &p, &i, &name) == 0);
                assert_se(family == f);
                assert_se(port == p);
                assert_se(ifindex == i);
                assert_se(streq_ptr(server_name, name));
                assert_se(in_addr_port_ifindex_name_to_string(f, &a, p, i, name, &x) >= 0);
                assert_se(streq(str, x));
        }

        if (port > 0)
                assert_se(in_addr_port_ifindex_name_from_string_auto(str, &f, &a, NULL, &i, &fake) == -EINVAL);
        else {
                _cleanup_free_ char *name = NULL, *x = NULL;
                assert_se(in_addr_port_ifindex_name_from_string_auto(str, &f, &a, NULL, &i, &name) == 0);
                assert_se(family == f);
                assert_se(ifindex == i);
                assert_se(streq_ptr(server_name, name));
                assert_se(in_addr_port_ifindex_name_to_string(f, &a, 0, i, name, &x) >= 0);
                assert_se(streq(str, x));
        }

        if (ifindex > 0)
                assert_se(in_addr_port_ifindex_name_from_string_auto(str, &f, &a, &p, NULL, &fake) == -EINVAL);
        else {
                _cleanup_free_ char *name = NULL, *x = NULL;
                assert_se(in_addr_port_ifindex_name_from_string_auto(str, &f, &a, &p, NULL, &name) == 0);
                assert_se(family == f);
                assert_se(port == p);
                assert_se(streq_ptr(server_name, name));
                assert_se(in_addr_port_ifindex_name_to_string(f, &a, p, 0, name, &x) >= 0);
                assert_se(streq(str, x));
        }

        if (server_name)
                assert_se(in_addr_port_ifindex_name_from_string_auto(str, &f, &a, &p, &i, NULL) == -EINVAL);
        else {
                _cleanup_free_ char *x = NULL;
                assert_se(in_addr_port_ifindex_name_from_string_auto(str, &f, &a, &p, &i, NULL) == 0);
                assert_se(family == f);
                assert_se(port == p);
                assert_se(ifindex == i);
                assert_se(in_addr_port_ifindex_name_to_string(f, &a, p, i, NULL, &x) >= 0);
                assert_se(streq(str, x));
        }
}

static void test_in_addr_port_ifindex_name_from_string_auto(void) {
        log_info("/* %s */", __func__);

        test_in_addr_port_ifindex_name_from_string_auto_one("192.168.0.1", AF_INET, 0, 0, NULL);
        test_in_addr_port_ifindex_name_from_string_auto_one("192.168.0.1#test.com", AF_INET, 0, 0, "test.com");
        test_in_addr_port_ifindex_name_from_string_auto_one("192.168.0.1:53", AF_INET, 53, 0, NULL);
        test_in_addr_port_ifindex_name_from_string_auto_one("192.168.0.1:53#example.com", AF_INET, 53, 0, "example.com");
        test_in_addr_port_ifindex_name_from_string_auto_one("fe80::18", AF_INET6, 0, 0, NULL);
        test_in_addr_port_ifindex_name_from_string_auto_one("fe80::18#hoge.com", AF_INET6, 0, 0, "hoge.com");
        test_in_addr_port_ifindex_name_from_string_auto_one("fe80::18%19", AF_INET6, 0, 19, NULL);
        test_in_addr_port_ifindex_name_from_string_auto_one("[fe80::18]:53", AF_INET6, 53, 0, NULL);
        test_in_addr_port_ifindex_name_from_string_auto_one("fe80::18%19#hoge.com", AF_INET6, 0, 19, "hoge.com");
        test_in_addr_port_ifindex_name_from_string_auto_one("[fe80::18]:53#hoge.com", AF_INET6, 53, 0, "hoge.com");
        test_in_addr_port_ifindex_name_from_string_auto_one("[fe80::18]:53%19", AF_INET6, 53, 19, NULL);
        test_in_addr_port_ifindex_name_from_string_auto_one("[fe80::18]:53%19#hoge.com", AF_INET6, 53, 19, "hoge.com");
}

int main(int argc, char *argv[]) {
        test_setup_logging(LOG_DEBUG);

        test_socket_address_parse();
        test_socket_address_parse_netlink();
        test_socket_address_equal();
        test_socket_address_get_path();
        test_socket_address_is();
        test_socket_address_is_netlink();

        test_in_addr_ifindex_to_string();
        test_in_addr_ifindex_from_string_auto();
        test_in_addr_ifindex_name_from_string_auto();
        test_in_addr_port_ifindex_name_from_string_auto();

        return 0;
}
