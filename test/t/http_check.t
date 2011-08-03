#
#===============================================================================
#
#         FILE:  http_check.t
#
#  DESCRIPTION: test 
#
#        FILES:  ---
#         BUGS:  ---
#        NOTES:  ---
#       AUTHOR:  Weibin Yao (http://yaoweibin.cn/), yaoweibin@gmail.com
#      COMPANY:  
#      VERSION:  1.0
#      CREATED:  03/02/2010 03:18:28 PM
#     REVISION:  ---
#===============================================================================


# vi:filetype=perl

use lib 'lib';
use Test::Nginx::LWP;

plan tests => repeat_each() * 2 * blocks();

#no_diff;

run_tests();

__DATA__

=== TEST 1: the http_check test-single server
--- config
    upstream test{
        server blog.163.com:80;
        check interval=3000 rise=1 fall=5 timeout=2000 type=http;
        check_http_send "GET / HTTP/1.0\r\n\r\n";
        check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 1982;

        location / {
            proxy_pass http://test;
        }
    }
--- request
GET /
--- response_body_like: ^<(.*)>$

=== TEST 2: the http_check test-multi_server
--- config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        #ip_hash;

        check interval=3000 rise=1 fall=5 timeout=2000 type=http;
        check_http_send "GET / HTTP/1.0\r\n\r\n";
        check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 1982;

        location / {
            proxy_pass http://test;
        }
    }
--- request
GET /
--- response_body_like: ^<(.*)>$

=== TEST 3: the http_check test
--- config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        #ip_hash;

        check interval=3000 rise=1 fall=5 timeout=2000 type=http;
        check_http_send "GET /foo HTTP/1.0\r\n\r\n";
        check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 1982;

        location / {
            proxy_pass http://test;
        }
    }
--- request
GET /
--- error_code: 502
--- response_body_like: ^.*$

=== TEST 4: the http_check without check directive
--- config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
    }

    server {
        listen 1982;

        location / {
            proxy_pass http://test;
        }
    }
--- request
GET /
--- response_body_like: ^<(.*)>$

=== TEST 5: the http_check which does not use the upstream
--- config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        #ip_hash;

        check interval=3000 rise=1 fall=5 timeout=2000 type=http;
        check_http_send "GET / HTTP/1.0\r\n\r\n";
        check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 1982;

        location / {
            proxy_pass http://blog.163.com;
        }
    }
--- request
GET /
--- response_body_like: ^<(.*)>$

=== TEST 6: the http_check test-single server
--- config
    upstream test{
        server blog.163.com:80;
        ip_hash;

        check interval=3000 rise=1 fall=5 timeout=2000 type=http;
        check_http_send "GET / HTTP/1.0\r\n\r\n";
        check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 1982;

        location / {
            proxy_pass http://test;
        }
    }
--- request
GET /
--- response_body_like: ^<(.*)>$

=== TEST 7: the http_check test-multi_server
--- config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        ip_hash;

        check interval=3000 rise=1 fall=5 timeout=2000 type=http;
        check_http_send "GET / HTTP/1.0\r\n\r\n";
        check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 1982;

        location / {
            proxy_pass http://test;
        }
    }
--- request
GET /
--- response_body_like: ^<(.*)>$

=== TEST 8: the http_check test
--- config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        ip_hash;

        check interval=3000 rise=1 fall=5 timeout=2000 type=http;
        check_http_send "GET /foo HTTP/1.0\r\n\r\n";
        check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 1982;

        location / {
            proxy_pass http://test;
        }
    }
--- request
GET /
--- error_code: 502
--- response_body_like: ^.*$

=== TEST 9: the http_check without check directive
--- config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        ip_hash;
    }

    server {
        listen 1982;

        location / {
            proxy_pass http://test;
        }
    }
--- request
GET /
--- response_body_like: ^<(.*)>$


=== TEST 10: the http_check which does not use the upstream
--- config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        ip_hash;

        check interval=3000 rise=1 fall=5 timeout=2000 type=http;
        check_http_send "GET / HTTP/1.0\r\n\r\n";
        check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 1982;

        location / {
            proxy_pass http://blog.163.com;
        }
    }
--- request
GET /
--- response_body_like: ^<(.*)>$
