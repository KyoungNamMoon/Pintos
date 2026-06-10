# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(memory-test-large) begin
(memory-test-large) allocated 12051 pages from kernel pool.
(memory-test-large) allocated 234916 pages from user pool.
(memory-test-large) allocated 12051 pages from kernel pool.
(memory-test-large) allocated 234916 pages from user pool.
(memory-test-large) allocated 12051 pages from kernel pool.
(memory-test-large) allocated 234916 pages from user pool.
(memory-test-large) PASS
(memory-test-large) end
EOF
pass;
