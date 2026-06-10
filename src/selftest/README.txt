
This folder builds miscellaneous self tests for several functionalities.

The tests are located in src/tests/self.

The tests are compiled into the kernel.  To run individual tests, you can do, for instance.

```
../utils/pintos --kvm -v -m 1024 -- -q run memory-test-multiple
../utils/pintos -v -- -q run console
```
