# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(memory-test-multiple) begin
(memory-test-multiple) allocated 122892 pages from kernel pool 2 at a time.
(memory-test-multiple) allocated 123640 pages from user pool 2 at a time.
(memory-test-multiple) allocated 123132 pages from kernel pool 4 at a time.
(memory-test-multiple) allocated 123640 pages from user pool 4 at a time.
(memory-test-multiple) allocated 123248 pages from kernel pool 8 at a time.
(memory-test-multiple) allocated 123640 pages from user pool 8 at a time.
(memory-test-multiple) allocated 123280 pages from kernel pool 16 at a time.
(memory-test-multiple) allocated 123632 pages from user pool 16 at a time.
(memory-test-multiple) allocated 123168 pages from kernel pool 32 at a time.
(memory-test-multiple) allocated 123616 pages from user pool 32 at a time.
(memory-test-multiple) allocated 122944 pages from kernel pool 64 at a time.
(memory-test-multiple) allocated 123584 pages from user pool 64 at a time.
(memory-test-multiple) allocated 122624 pages from kernel pool 128 at a time.
(memory-test-multiple) allocated 123520 pages from user pool 128 at a time.
(memory-test-multiple) allocated 122112 pages from kernel pool 256 at a time.
(memory-test-multiple) allocated 123392 pages from user pool 256 at a time.
(memory-test-multiple) allocated 121344 pages from kernel pool 512 at a time.
(memory-test-multiple) allocated 123392 pages from user pool 512 at a time.
(memory-test-multiple) PASS
(memory-test-multiple) end
EOF
pass;
