# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_EXIT_CODES => 1, [<<'EOF']);
(orphan-wait) begin
(orphan-wait) end
EOF
pass;
