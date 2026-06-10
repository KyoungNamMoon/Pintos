# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(memory-test-medium) begin
(memory-test-medium) allocated 7980 pages from kernel pool.
(memory-test-medium) allocated 8047 pages from user pool.
(memory-test-medium) allocated 7980 pages from kernel pool.
(memory-test-medium) allocated 8047 pages from user pool.
(memory-test-medium) allocated 7980 pages from kernel pool.
(memory-test-medium) allocated 8047 pages from user pool.
(memory-test-medium) PASS
(memory-test-medium) end
EOF
pass;
