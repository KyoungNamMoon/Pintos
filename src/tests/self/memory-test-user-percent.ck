# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(memory-test-user-percent) begin
(memory-test-user-percent) allocated 3158 pages from kernel pool.
(memory-test-user-percent) allocated 28999 pages from user pool.
(memory-test-user-percent) allocated 3158 pages from kernel pool.
(memory-test-user-percent) allocated 28999 pages from user pool.
(memory-test-user-percent) allocated 3158 pages from kernel pool.
(memory-test-user-percent) allocated 28999 pages from user pool.
(memory-test-user-percent) PASS
(memory-test-user-percent) end
EOF
pass;
