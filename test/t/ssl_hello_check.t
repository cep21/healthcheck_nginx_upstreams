#
#===============================================================================
#
#         FILE:  ssl_hello_check.t
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

=== TEST 1: the ssl_hello_check test
--- http_config
    upstream test{
        server www.alipay.com:443;
        server www.alipay.com:444;
        server www.alipay.com:445;

        check interval=4000 rise=1 fall=5 timeout=2000 type=ssl_hello;
    }

--- config
    location / {
        proxy_pass https://test;
    }
   
--- request
GET /
--- response_body_like: ^<(.*)>[\r\n\s\t]*$

=== TEST 2: the ssl_hello_check test with ip_hash
--- http_config
    upstream test{
        server www.alipay.com:443;
        server www.alipay.com:444;
        server www.alipay.com:445;
        ip_hash;

        check interval=4000 rise=1 fall=5 timeout=2000 type=ssl_hello;
    }

--- config
    location / {
        proxy_pass https://test;
    }

--- request
GET /
--- response_body_like: ^<(.*)>[\r\n\s\t]*$

=== TEST 3: the ssl_hello_check test with bad ip
--- http_config
    upstream test{
        server www.alipay.com:80;
        server www.alipay.com:443;
        server www.alipay.com:444;
        server www.alipay.com:445;

        check interval=4000 rise=1 fall=5 timeout=2000 type=ssl_hello;
    }

--- config
    location / {
        proxy_pass https://test;
    }

--- request
GET /
--- response_body_like: ^<(.*)>[\r\n\s\t]*$

=== TEST 4: the ssl_hello_check test with least_conn
--- skip_nginx
2: < 1.2.2
--- http_config
    upstream test{
        server www.alipay.com:443;
        server www.alipay.com:444;
        server www.alipay.com:445;
        least_conn;

        check interval=4000 rise=1 fall=5 timeout=2000 type=ssl_hello;
    }

--- config
    location / {
        proxy_pass https://test;
    }

--- request
GET /
--- response_body_like: ^<(.*)>[\r\n\s\t]*$

