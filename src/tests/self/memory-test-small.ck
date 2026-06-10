# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(memory-test-small) begin
(memory-test-small) allocated 361 pages from kernel pool.
(memory-test-small) allocated 351 pages from user pool.
(memory-test-small) allocated 361 pages from kernel pool.
(memory-test-small) allocated 351 pages from user pool.
(memory-test-small) allocated 361 pages from kernel pool.
(memory-test-small) allocated 351 pages from user pool.
(memory-test-small) PASS
(memory-test-small) end
EOF
pass;
