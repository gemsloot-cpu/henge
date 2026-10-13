/* M1 sample: exercises plain calls, GOT access (GLOB_DAT), pointer tables in
 * data (RELATIVE / ABS64), and DT_INIT_ARRAY. Built with the Android NDK. */

int add(int a, int b) { return a + b; }

int sub(int a, int b) { return a - b; }

/* Non-static, default visibility: under -fPIC it is preemptible, so accesses
 * go through the GOT (R_AARCH64_GLOB_DAT). */
int counter = 40;

int bump(void) { return ++counter; }

/* Function pointers stored in data need a dynamic relocation each. */
static int (*ops[])(int, int) = {add, sub};

int apply(int i, int a, int b) { return ops[i](a, b); }

/* Set by a constructor; verifies DT_INIT_ARRAY ran. */
int init_ran;

__attribute__((constructor)) static void libadd_init(void) {
    /* The empty asm stops clang's GlobalOpt from folding this constructor into
     * a static initializer (which would drop DT_INIT_ARRAY entirely). */
    __asm__ volatile("" ::: "memory");
    init_ran = 1;
}

int was_initialized(void) { return init_ran; }
