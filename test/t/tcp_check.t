#
#===============================================================================
#
#         FILE:  tcp_check.t
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

plan tests => repeat_each(2) * 2 * blocks();

no_root_location();
#no_diff;

run_tests();

__DATA__

=== TEST 1: the tcp_check test
--- http_config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        server blog.163.com:82;

        check interval=3000 rise=1 fall=5 timeout=1000;
    }

--- config
    location / { 
        proxy_pass http://test;
    }

--- request
GET /
--- response_body_like: ^<(.*)>$

=== TEST 2: the tcp_check test with ip_hash
--- http_config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        server blog.163.com:82;
        ip_hash;

        check interval=3000 rise=1 fall=5 timeout=1000 type=tcp;
    }

--- config
    location / { 
        proxy_pass http://test;
    }

--- request
GET /
--- response_body_like: ^<(.*)>$

=== TEST 3: the tcp_check test which don't use the checked upstream
--- http_config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        server blog.163.com:82;

        check interval=3000 rise=1 fall=5 timeout=1000;
    }

--- config
    location / {
        proxy_pass http://blog.163.com;
    }

--- request
GET /
--- response_body_like: ^<(.*)>$

=== TEST 3: the tcp_check test with least_conn;
--- skip_nginx
2: < 1.2.2
--- http_config
    upstream test{
        server blog.163.com:80;
        server blog.163.com:81;
        server blog.163.com:82;
        least_conn;

        check interval=3000 rise=1 fall=5 timeout=1000 type=tcp;
    }

--- config
    location / { 
        proxy_pass http://test;
    }

--- request
GET /
--- response_body_like: ^<(.*)>$

