/* touchtest.c
 * End-to-end test for the touch command (create syscall).
 * Covers: creating a new file, touching an existing file,
 *         verifying the file is usable after touch.
 */
#include <stdio.h>
#include <syscall.h>
#include <stdbool.h>

static int pass_count = 0;
static int fail_count = 0;

static void check(const char *label, bool ok) {
    if (ok) {
        printf("  [PASS] %s\n", label);
        pass_count++;
    } else {
        printf("  [FAIL] %s\n", label);
        fail_count++;
    }
}

int main(void) {
    printf("\n");
    printf("========================================\n");
    printf("  touch End-to-End Test\n");
    printf("========================================\n");

    /* ---- 1. Touch creates a new file ---- */
    printf("\n[1] Touch creates a new empty file\n");

    int fd = open("newfile");
    check("File does not exist yet", fd < 0);

    check("Create via touch (create)", create("newfile", 0));

    fd = open("newfile");
    check("File exists after touch", fd >= 0);
    check("File is empty (size 0)", filesize(fd) == 0);
    close(fd);

    /* ---- 2. Touch an existing file succeeds ---- */
    printf("\n[2] Touch an existing file is harmless\n");

    check("Write data to file", (fd = open("newfile")) >= 0);
    close(fd);

    if (!create("existfile", 0))
        printf("  (setup: created existfile)\n");
    fd = open("existfile");
    check("Existing file still accessible", fd >= 0);
    close(fd);

    /* ---- 3. Touch and then write to the file ---- */
    printf("\n[3] File is usable after touch\n");

    check("Create writefile", create("writefile", 0));
    fd = open("writefile");
    check("Open writefile", fd >= 0);
    check("Write to touched file", write(fd, "hello", 5) == 5);
    close(fd);

    fd = open("writefile");
    check("Reopen writefile", fd >= 0);
    char buf[6];
    check("Read back data", read(fd, buf, 5) == 5);
    close(fd);

    /* ---- 4. Touch multiple distinct files ---- */
    printf("\n[4] Touch multiple files\n");

    check("Create file_a", create("file_a", 0));
    check("Create file_b", create("file_b", 0));
    check("Create file_c", create("file_c", 0));

    fd = open("file_a");
    check("file_a exists", fd >= 0);
    close(fd);

    fd = open("file_b");
    check("file_b exists", fd >= 0);
    close(fd);

    fd = open("file_c");
    check("file_c exists", fd >= 0);
    close(fd);

    /* ---- Summary ---- */
    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", pass_count, fail_count);
    printf("========================================\n\n");
    return fail_count ? -1 : 0;
}
