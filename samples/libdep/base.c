/* Dependency of libdep.so. Its pointer table makes many RELATIVE relocations, which the
 * build packs as APS2 (DT_ANDROID_RELA). */
int base_counter = 7;

int base_value(void) { return 1234; }

static const char *const names[] = {"zero", "one", "two", "three", "four", "five", "six", "seven",
                                    "eight", "nine", "ten", "eleven", "twelve", "thirteen"};
const char *base_name(int i) { return names[i]; }
